#include "V2Transfer.h"
#include "ProtocolV2.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace v2transfer {
namespace {
using protocolv2::MessageType;
namespace fs = std::filesystem;
constexpr uint64_t kMaxFrameBytes = 200ULL * 1024 * 1024;
constexpr uint32_t kChunkBytes = 64 * 1024;
constexpr uint32_t kMaxMessageBytes = kChunkBytes + 24;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kBadSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { if (s != kBadSocket) closesocket(s); }
bool initSockets() {
    static bool ok = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    return ok;
}
#else
using SocketHandle = int;
constexpr SocketHandle kBadSocket = -1;
void closeSocket(SocketHandle s) { if (s != kBadSocket) ::close(s); }
bool initSockets() { return true; }
#endif

class Socket {
public:
    Socket() = default;
    explicit Socket(SocketHandle s) : handle_(s) { setTimeout(); }
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    bool valid() const { return handle_ != kBadSocket; }
    SocketHandle get() const { return handle_; }
    void reset(SocketHandle s) { close(); handle_ = s; setTimeout(); }
    void close() { closeSocket(handle_); handle_ = kBadSocket; }
    bool sendAll(const uint8_t* data, size_t length) {
        while (length) {
            int n = ::send(handle_, reinterpret_cast<const char*>(data),
                           static_cast<int>(std::min<size_t>(length, 1 << 20)), 0);
            if (n <= 0) return false;
            data += n; length -= static_cast<size_t>(n);
        }
        return true;
    }
    bool recvAll(uint8_t* data, size_t length) {
        while (length) {
            int n = ::recv(handle_, reinterpret_cast<char*>(data),
                           static_cast<int>(std::min<size_t>(length, 1 << 20)), 0);
            if (n <= 0) return false;
            data += n; length -= static_cast<size_t>(n);
        }
        return true;
    }
private:
    void setTimeout() {
        if (!valid()) return;
#ifdef _WIN32
        DWORD ms = 3000;
        setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
        setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
        timeval tv{3, 0};
        setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }
    SocketHandle handle_ = kBadSocket;
};

bool connectTo(Socket& socket, const std::string& host, uint16_t port) {
    if (!initSockets()) return false;
    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBadSocket) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(s); return false;
    }
    socket.reset(s);
    return true;
}

SocketHandle listenOn(uint16_t port) {
    if (!initSockets()) return kBadSocket;
    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBadSocket) return s;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 8) != 0) {
        closeSocket(s); return kBadSocket;
    }
    return s;
}

bool read16(const std::vector<uint8_t>& data, size_t& at, uint16_t& out) {
    if (at + 2 > data.size()) return false;
    out = (uint16_t(data[at]) << 8) | data[at + 1]; at += 2; return true;
}
bool read32(const std::vector<uint8_t>& data, size_t& at, uint32_t& out) {
    if (at + 4 > data.size()) return false;
    out = (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
          (uint32_t(data[at + 2]) << 8) | data[at + 3]; at += 4; return true;
}
bool read64(const std::vector<uint8_t>& data, size_t& at, uint64_t& out) {
    if (at + 8 > data.size()) return false;
    out = 0; for (int i = 0; i < 8; ++i) out = (out << 8) | data[at++]; return true;
}
void appendDouble(std::vector<uint8_t>& data, double value) {
    static_assert(sizeof(double) == sizeof(uint64_t), "64-bit IEEE double required");
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    protocolv2::appendUint64(data, bits);
}
bool readDouble(const std::vector<uint8_t>& data, size_t& at, double& out) {
    uint64_t bits = 0;
    if (!read64(data, at, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

std::vector<uint8_t> metaBytes(const Frame& f) {
    std::vector<uint8_t> b;
    protocolv2::appendUint64(b, f.taskId);
    protocolv2::appendUint32(b, f.lineId);
    protocolv2::appendUint32(b, f.attemptId);
    protocolv2::appendUint32(b, f.frameIndex);
    protocolv2::appendUint64(b, f.frameSeq);
    appendDouble(b, f.stageX);
    appendDouble(b, f.stageY);
    protocolv2::appendUint32(b, f.rows);
    protocolv2::appendUint32(b, f.cols);
    protocolv2::appendUint32(b, f.pixelType);
    protocolv2::appendUint32(b, f.elemSize);
    protocolv2::appendUint64(b, f.pixels.size());
    protocolv2::appendUint32(b, kChunkBytes);
    protocolv2::appendUint32(b, static_cast<uint32_t>((f.pixels.size() + kChunkBytes - 1) / kChunkBytes));
    protocolv2::appendUint32(b, crc32(f.pixels.data(), f.pixels.size()));
    return b;
}

bool parseMeta(const std::vector<uint8_t>& b, Frame& f, uint32_t& chunks, uint32_t& crc) {
    if (b.size() != 80) return false;
    size_t at = 0; uint64_t len = 0; uint32_t chunkSize = 0;
    if (!read64(b, at, f.taskId) || !read32(b, at, f.lineId) ||
        !read32(b, at, f.attemptId) || !read32(b, at, f.frameIndex) ||
        !read64(b, at, f.frameSeq) || !readDouble(b, at, f.stageX) ||
        !readDouble(b, at, f.stageY) || !read32(b, at, f.rows) ||
        !read32(b, at, f.cols) || !read32(b, at, f.pixelType) ||
        !read32(b, at, f.elemSize) || !read64(b, at, len) ||
        !read32(b, at, chunkSize) || !read32(b, at, chunks) || !read32(b, at, crc)) return false;
    if (!f.taskId || !f.frameSeq || !f.rows || !f.cols || !f.elemSize ||
        !len || len > kMaxFrameBytes || chunkSize != kChunkBytes ||
        chunks != (len + kChunkBytes - 1) / kChunkBytes) return false;
    f.pixels.resize(static_cast<size_t>(len));
    return true;
}

bool writeMessage(Socket& socket, MessageType type, const std::vector<uint8_t>& body) {
    auto header = protocolv2::makeMessageHeader(type, static_cast<uint32_t>(body.size()));
    return socket.sendAll(header.data(), header.size()) &&
           (body.empty() || socket.sendAll(body.data(), body.size()));
}
bool readMessage(Socket& socket, MessageType& type, std::vector<uint8_t>& body) {
    std::vector<uint8_t> header(12);
    if (!socket.recvAll(header.data(), header.size())) return false;
    size_t at = 0; uint32_t magic = 0, length = 0; uint16_t version = 0, rawType = 0;
    if (!read32(header, at, magic) || !read16(header, at, version) ||
        !read16(header, at, rawType) || !read32(header, at, length) ||
        magic != protocolv2::kMagic || version != protocolv2::kVersion || length > kMaxMessageBytes) return false;
    type = static_cast<MessageType>(rawType);
    body.resize(length);
    return !length || socket.recvAll(body.data(), length);
}
std::vector<uint8_t> two64(uint64_t first, uint64_t second) {
    std::vector<uint8_t> b; protocolv2::appendUint64(b, first); protocolv2::appendUint64(b, second); return b;
}
bool parseTwo64(const std::vector<uint8_t>& b, uint64_t& first, uint64_t& second) {
    size_t at = 0; return b.size() == 16 && read64(b, at, first) && read64(b, at, second);
}

fs::path framePath(const fs::path& root, uint64_t task, uint64_t seq) {
    return root / std::to_string(task) / (std::to_string(seq) + ".frame");
}
bool durableWrite(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    fs::path tmp = path; tmp += ".part";
#ifdef _WIN32
    FILE* file = nullptr;
    if (_wfopen_s(&file, tmp.c_str(), L"wb") != 0) return false;
#else
    FILE* file = fopen(tmp.c_str(), "wb");
    if (!file) return false;
#endif
    bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
              fflush(file) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(file)) == 0;
#else
    ok = ok && fsync(fileno(file)) == 0;
#endif
    ok = fclose(file) == 0 && ok;
    if (!ok) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}
bool persistFrame(const fs::path& path, const Frame& f) {
    if (f.pixels.empty() || f.pixels.size() > kMaxFrameBytes) return false;
    auto b = metaBytes(f);
    b.insert(b.end(), f.pixels.begin(), f.pixels.end());
    return durableWrite(path, b);
}

std::vector<fs::path> pendingPaths(const fs::path& root, uint64_t task) {
    std::vector<fs::path> paths;
    std::error_code ec; fs::path dir = root / std::to_string(task);
    if (!fs::exists(dir, ec)) return paths;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".frame") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) {
        return std::stoull(a.stem().string()) < std::stoull(b.stem().string());
    });
    return paths;
}
uint64_t contiguousDurable(const fs::path& root, uint64_t task) {
    uint64_t seq = 0;
    for (const auto& path : pendingPaths(root, task)) {
        uint64_t current = std::stoull(path.stem().string());
        if (current == seq + 1) seq = current;
        else if (current > seq + 1) break;
    }
    return seq;
}
} // namespace

Frame fromRawFrame(const RawFrame& raw, uint64_t taskId) {
    Frame f;
    f.taskId = taskId; f.frameSeq = raw.frameId; f.frameIndex = static_cast<uint32_t>(raw.frameId - 1);
    f.rows = raw.rows; f.cols = raw.cols; f.pixelType = raw.type; f.elemSize = raw.elemSize;
    if (!raw.empty()) f.pixels.assign(raw.data(), raw.data() + raw.totalBytes);
    return f;
}

bool loadFrame(const fs::path& path, Frame& f) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    auto size = in.tellg();
    if (size < 81 || size > static_cast<std::streamoff>(kMaxFrameBytes + 80)) return false;
    in.seekg(0);
    std::vector<uint8_t> b(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(b.data()), size)) return false;
    std::vector<uint8_t> meta(b.begin(), b.begin() + 80);
    uint32_t chunks = 0, crc = 0;
    if (!parseMeta(meta, f, chunks, crc) || b.size() != 80 + f.pixels.size()) return false;
    std::copy(b.begin() + 80, b.end(), f.pixels.begin());
    return crc32(f.pixels.data(), f.pixels.size()) == crc;
}

struct Sender::Impl { Socket socket; };
Sender::Sender(std::string host, uint16_t port, fs::path spoolDir, uint64_t taskId)
    : impl_(new Impl), spoolDir_(std::move(spoolDir)), host_(std::move(host)), port_(port), taskId_(taskId) {}
Sender::~Sender() = default;
size_t Sender::pendingCount() const { return pendingPaths(spoolDir_, taskId_).size(); }
bool Sender::persist(const Frame& f) {
    if (f.taskId != taskId_ || !f.frameSeq || !f.rows || !f.cols || !f.elemSize || f.pixels.empty()) {
        error_ = "invalid frame"; return false;
    }
    const fs::path path = framePath(spoolDir_, taskId_, f.frameSeq);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        Frame saved;
        if (loadFrame(path, saved) && saved.pixels == f.pixels && saved.lineId == f.lineId &&
            saved.attemptId == f.attemptId && saved.stageX == f.stageX && saved.stageY == f.stageY) return true;
        error_ = "frame sequence already has different spooled data";
        return false;
    }
    if (!persistFrame(path, f)) {
        error_ = "cannot persist sender frame"; return false;
    }
    return true;
}

bool Sender::attemptResume() {
    impl_->socket.close();
    if (!connectTo(impl_->socket, host_, port_)) { error_ = "connect failed"; return false; }
    std::vector<uint8_t> one; protocolv2::appendUint64(one, taskId_);
    MessageType type{}; std::vector<uint8_t> body;
    if (!writeMessage(impl_->socket, MessageType::Hello, one) ||
        !writeMessage(impl_->socket, MessageType::ResumeRequest, one) ||
        !readMessage(impl_->socket, type, body) || type != MessageType::ResumeReply) {
        error_ = "resume handshake failed"; return false;
    }
    uint64_t task = 0, durable = 0;
    if (!parseTwo64(body, task, durable) || task != taskId_) {
        error_ = "wrong resume reply"; return false;
    }
    nextFrameSeq_ = std::max(nextFrameSeq_, durable + 1);
    for (const auto& path : pendingPaths(spoolDir_, taskId_)) {
        Frame f;
        if (!loadFrame(path, f)) { error_ = "corrupt sender spool"; return false; }
        if (f.frameSeq <= durable) { std::error_code ec; fs::remove(path, ec); if (ec) return false; continue; }
        auto meta = metaBytes(f);
        if (!writeMessage(impl_->socket, MessageType::FrameBegin, meta)) {
            error_ = "frame begin failed"; return false;
        }
        for (size_t offset = 0, index = 0; offset < f.pixels.size(); offset += kChunkBytes, ++index) {
            uint32_t len = static_cast<uint32_t>(std::min<size_t>(kChunkBytes, f.pixels.size() - offset));
            std::vector<uint8_t> chunk;
            protocolv2::appendUint64(chunk, f.frameSeq);
            protocolv2::appendUint32(chunk, static_cast<uint32_t>(index));
            protocolv2::appendUint64(chunk, offset);
            protocolv2::appendUint32(chunk, len);
            chunk.insert(chunk.end(), f.pixels.begin() + offset, f.pixels.begin() + offset + len);
            if (!writeMessage(impl_->socket, MessageType::FrameChunk, chunk)) {
                error_ = "frame chunk failed"; return false;
            }
        }
        if (!readMessage(impl_->socket, type, body)) { error_ = "ACK missing"; return false; }
        uint64_t replyTask = 0, replySeq = 0;
        if (!parseTwo64(body, replyTask, replySeq) || replyTask != taskId_ || replySeq != f.frameSeq ||
            type != MessageType::Ack) { error_ = "NACK or invalid ACK"; return false; }
        std::error_code ec; fs::remove(path, ec);
        if (ec) { error_ = "cannot delete ACKed spool"; return false; }
        nextFrameSeq_ = std::max(nextFrameSeq_, f.frameSeq + 1);
    }
    return true;
}
bool Sender::resume(unsigned attempts) {
    for (unsigned n = 0; n < attempts; ++n) {
        if (attemptResume()) { error_.clear(); return true; }
        impl_->socket.close();
        if (n + 1 < attempts) std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}
bool Sender::send(const Frame& f, unsigned attempts) { return persist(f) && resume(attempts); }
bool Sender::finish(unsigned attempts) {
    if (!resume(attempts)) return false;
    std::vector<uint8_t> body;
    protocolv2::appendUint64(body, taskId_);
    if (!writeMessage(impl_->socket, MessageType::TaskFinish, body)) {
        error_ = "task finish send failed";
        return false;
    }
    MessageType type{};
    std::vector<uint8_t> reply;
    uint64_t task = 0, seq = 1;
    if (!readMessage(impl_->socket, type, reply) || type != MessageType::Ack ||
        !parseTwo64(reply, task, seq) || task != taskId_ || seq != 0) {
        error_ = "task finish ACK missing";
        return false;
    }
    error_.clear();
    return true;
}

struct Receiver::Impl {
    Socket listener;
    Socket client;
    std::atomic<bool> stopped{false};
    std::atomic<bool> finished{false};
};
Receiver::Receiver(uint16_t port, fs::path spoolDir)
    : impl_(new Impl), spoolDir_(std::move(spoolDir)) {
    impl_->listener.reset(listenOn(port));
    if (!impl_->listener.valid()) error_ = "listen failed";
}
Receiver::~Receiver() { stop(); }
void Receiver::setFrameHandler(FrameHandler handler) { handler_ = std::move(handler); }
void Receiver::setFinishHandler(FinishHandler handler) { finishHandler_ = std::move(handler); }
void Receiver::dropNextAckForTest() { dropAck_ = true; }
void Receiver::disconnectNextFrameForTest() { disconnectFrame_ = true; }
void Receiver::stop() {
    impl_->stopped.store(true);
    impl_->client.close(); impl_->listener.close();
}
bool Receiver::serveOne() {
    if (!impl_->listener.valid()) return false;
    SocketHandle s = ::accept(impl_->listener.get(), nullptr, nullptr);
    if (s == kBadSocket) return false;
    impl_->client.reset(s);
    bool ok = handleClient();
    impl_->client.close();
    return ok;
}
bool Receiver::serveForever() {
    if (!impl_->listener.valid()) return false;
    while (!impl_->stopped.load()) serveOne();
    return true;
}
bool Receiver::serveUntilFinished() {
    if (!impl_->listener.valid()) return false;
    while (!impl_->stopped.load() && !impl_->finished.load()) {
        serveOne();
    }
    return impl_->finished.load();
}

bool Receiver::handleClient() {
    MessageType type{}; std::vector<uint8_t> body; uint64_t task = 0;
    if (!readMessage(impl_->client, type, body) || type != MessageType::Hello || body.size() != 8) return false;
    size_t at = 0; if (!read64(body, at, task) || !task) return false;
    if (!readMessage(impl_->client, type, body) || type != MessageType::ResumeRequest || body.size() != 8) return false;
    at = 0; uint64_t requested = 0;
    if (!read64(body, at, requested) || requested != task) return false;
    if (!writeMessage(impl_->client, MessageType::ResumeReply, two64(task, contiguousDurable(spoolDir_, task)))) return false;
    while (!impl_->stopped.load() && readMessage(impl_->client, type, body)) {
        if (type == MessageType::TaskFinish) {
            size_t pos = 0;
            uint64_t finishedTask = 0;
            if (body.size() != 8 || !read64(body, pos, finishedTask) || finishedTask != task) return false;
            if (finishHandler_) finishHandler_(task);
            if (!writeMessage(impl_->client, MessageType::Ack, two64(task, 0))) return false;
            impl_->finished.store(true);
            return true;
        }
        if (type != MessageType::FrameBegin) return false;
        Frame f; uint32_t chunks = 0, expectedCrc = 0;
        if (!parseMeta(body, f, chunks, expectedCrc) || f.taskId != task) return false;
        fs::path path = framePath(spoolDir_, task, f.frameSeq);
        if (disconnectFrame_.exchange(false)) return false;
        for (uint32_t i = 0; i < chunks; ++i) {
            if (!readMessage(impl_->client, type, body) || type != MessageType::FrameChunk) return false;
            size_t pos = 0; uint64_t seq = 0, offset = 0; uint32_t index = 0, len = 0;
            if (!read64(body, pos, seq) || !read32(body, pos, index) ||
                !read64(body, pos, offset) || !read32(body, pos, len) ||
                seq != f.frameSeq || index != i || offset != uint64_t(i) * kChunkBytes ||
                !len || len > kChunkBytes || offset + len > f.pixels.size() || pos + len != body.size()) return false;
            std::copy(body.begin() + pos, body.end(), f.pixels.begin() + offset);
        }
        if (crc32(f.pixels.data(), f.pixels.size()) != expectedCrc) {
            writeMessage(impl_->client, MessageType::Nack, two64(task, f.frameSeq));
            continue;
        }
        std::error_code ec; bool already = fs::exists(path, ec);
        if (already) {
            Frame saved;
            if (!loadFrame(path, saved) || saved.pixels != f.pixels || saved.lineId != f.lineId ||
                saved.attemptId != f.attemptId || saved.stageX != f.stageX || saved.stageY != f.stageY) return false;
        } else {
            if (!persistFrame(path, f)) { error_ = "receiver durable write failed"; return false; }
            if (handler_) handler_(f);
        }
        if (dropAck_.exchange(false)) return false;
        if (!writeMessage(impl_->client, MessageType::Ack, two64(task, f.frameSeq))) return false;
    }
    return true;
}
} // namespace v2transfer
