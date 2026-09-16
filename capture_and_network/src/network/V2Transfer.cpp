#include "V2Transfer.h"
#include "ProtocolV2.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
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
#endif

namespace v2transfer {
namespace {
namespace fs = std::filesystem;
using protocolv2::MessageType;
constexpr uint64_t kMaxFrameBytes = 200ULL * kMiB;
constexpr uint32_t kChunkBytes = 64 * 1024;
constexpr uint32_t kMaxMessageBytes = kChunkBytes + 24;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kBadSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { if (s != kBadSocket) closesocket(s); }
bool initSockets() { static bool ok = [] { WSADATA d{}; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }(); return ok; }
#else
using SocketHandle = int;
constexpr SocketHandle kBadSocket = -1;
void closeSocket(SocketHandle s) { if (s != kBadSocket) ::close(s); }
bool initSockets() { return true; }
#endif

class Socket {
public:
    ~Socket() { close(); }
    bool valid() const { return handle_ != kBadSocket; }
    SocketHandle get() const { return handle_; }
    void reset(SocketHandle s) { close(); handle_ = s; setTimeout(); }
    void close() {
        SocketHandle old = handle_;
        handle_ = kBadSocket;
        if (old != kBadSocket) {
#ifdef _WIN32
            shutdown(old, SD_BOTH);
#else
            shutdown(old, SHUT_RDWR);
#endif
            closeSocket(old);
        }
    }
    bool sendAll(const uint8_t* p, size_t n) {
        while (n && valid()) {
            int sent = ::send(handle_, reinterpret_cast<const char*>(p), static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
            if (sent <= 0) return false;
            p += sent; n -= static_cast<size_t>(sent);
        }
        return n == 0;
    }
    bool recvAll(uint8_t* p, size_t n) {
        while (n && valid()) {
            int got = ::recv(handle_, reinterpret_cast<char*>(p), static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
            if (got <= 0) return false;
            p += got; n -= static_cast<size_t>(got);
        }
        return n == 0;
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
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(s); return false;
    }
    socket.reset(s); return true;
}

SocketHandle listenOn(uint16_t port) {
    if (!initSockets()) return kBadSocket;
    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBadSocket) return s;
    int yes = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 8) != 0) {
        closeSocket(s); return kBadSocket;
    }
    return s;
}

bool read16(const std::vector<uint8_t>& b, size_t& at, uint16_t& v) {
    if (at + 2 > b.size()) return false; v = (uint16_t(b[at]) << 8) | b[at + 1]; at += 2; return true;
}
bool read32(const std::vector<uint8_t>& b, size_t& at, uint32_t& v) {
    if (at + 4 > b.size()) return false;
    v = (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3]; at += 4; return true;
}
bool read64(const std::vector<uint8_t>& b, size_t& at, uint64_t& v) {
    if (at + 8 > b.size()) return false; v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | b[at++]; return true;
}
void appendDouble(std::vector<uint8_t>& b, double v) {
    uint64_t bits = 0; std::memcpy(&bits, &v, sizeof(bits)); protocolv2::appendUint64(b, bits);
}
bool readDouble(const std::vector<uint8_t>& b, size_t& at, double& v) {
    uint64_t bits = 0; if (!read64(b, at, bits)) return false; std::memcpy(&v, &bits, sizeof(v)); return true;
}
uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) { crc ^= p[i]; for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u); }
    return ~crc;
}

std::vector<uint8_t> metaBytes(const Frame& f) {
    std::vector<uint8_t> b;
    protocolv2::appendUint64(b, f.taskId); protocolv2::appendUint32(b, f.lineId);
    protocolv2::appendUint32(b, f.attemptId); protocolv2::appendUint32(b, f.frameIndex);
    protocolv2::appendUint64(b, f.frameSeq); appendDouble(b, f.stageX); appendDouble(b, f.stageY);
    protocolv2::appendUint32(b, f.rows); protocolv2::appendUint32(b, f.cols);
    protocolv2::appendUint32(b, f.pixelType); protocolv2::appendUint32(b, f.elemSize);
    protocolv2::appendUint64(b, f.pixels.size()); protocolv2::appendUint32(b, kChunkBytes);
    protocolv2::appendUint32(b, static_cast<uint32_t>((f.pixels.size() + kChunkBytes - 1) / kChunkBytes));
    protocolv2::appendUint32(b, crc32(f.pixels.data(), f.pixels.size())); return b;
}
bool parseMeta(const std::vector<uint8_t>& b, Frame& f, uint32_t& chunks, uint32_t& crc) {
    if (b.size() != 80) return false;
    size_t at = 0; uint64_t len = 0; uint32_t chunkSize = 0;
    if (!read64(b, at, f.taskId) || !read32(b, at, f.lineId) || !read32(b, at, f.attemptId) ||
        !read32(b, at, f.frameIndex) || !read64(b, at, f.frameSeq) || !readDouble(b, at, f.stageX) ||
        !readDouble(b, at, f.stageY) || !read32(b, at, f.rows) || !read32(b, at, f.cols) ||
        !read32(b, at, f.pixelType) || !read32(b, at, f.elemSize) || !read64(b, at, len) ||
        !read32(b, at, chunkSize) || !read32(b, at, chunks) || !read32(b, at, crc)) return false;
    if (!f.taskId || !f.frameSeq || !f.rows || !f.cols || !f.elemSize || !len || len > kMaxFrameBytes ||
        chunkSize != kChunkBytes || chunks != (len + kChunkBytes - 1) / kChunkBytes) return false;
    f.pixels.resize(static_cast<size_t>(len)); return true;
}
bool validFrame(const Frame& f, uint64_t task) {
    if (f.taskId != task || !f.frameSeq || !f.rows || !f.cols || !f.elemSize || f.pixels.empty() || f.pixels.size() > kMaxFrameBytes) return false;
    const uint64_t cells = uint64_t(f.rows) * f.cols;
    return cells <= kMaxFrameBytes && f.elemSize <= kMaxFrameBytes && cells * f.elemSize == f.pixels.size();
}
bool sameFrame(const Frame& a, const Frame& b) {
    return a.taskId == b.taskId && a.lineId == b.lineId && a.attemptId == b.attemptId &&
           a.frameIndex == b.frameIndex && a.frameSeq == b.frameSeq && a.stageX == b.stageX &&
           a.stageY == b.stageY && a.rows == b.rows && a.cols == b.cols && a.pixelType == b.pixelType &&
           a.elemSize == b.elemSize && a.pixels == b.pixels;
}

bool writeMessage(Socket& s, MessageType type, const std::vector<uint8_t>& body) {
    auto h = protocolv2::makeMessageHeader(type, static_cast<uint32_t>(body.size()));
    return s.sendAll(h.data(), h.size()) && (body.empty() || s.sendAll(body.data(), body.size()));
}
bool readMessage(Socket& s, MessageType& type, std::vector<uint8_t>& body) {
    std::vector<uint8_t> h(12); if (!s.recvAll(h.data(), h.size())) return false;
    size_t at = 0; uint32_t magic = 0, len = 0; uint16_t version = 0, raw = 0;
    if (!read32(h, at, magic) || !read16(h, at, version) || !read16(h, at, raw) || !read32(h, at, len) ||
        magic != protocolv2::kMagic || version != protocolv2::kVersion || len > kMaxMessageBytes) return false;
    type = static_cast<MessageType>(raw); body.resize(len); return !len || s.recvAll(body.data(), len);
}
std::vector<uint8_t> two64(uint64_t a, uint64_t b) {
    std::vector<uint8_t> out; protocolv2::appendUint64(out, a); protocolv2::appendUint64(out, b); return out;
}
std::vector<uint8_t> nackBody(uint64_t task, uint64_t seq, NackReason reason) {
    auto out = two64(task, seq); protocolv2::appendUint32(out, static_cast<uint32_t>(reason)); return out;
}
bool parseReply(const std::vector<uint8_t>& b, uint64_t& task, uint64_t& seq, NackReason* reason = nullptr) {
    if (b.size() != 16 && b.size() != 20) return false;
    size_t at = 0; if (!read64(b, at, task) || !read64(b, at, seq)) return false;
    if (reason) { uint32_t raw = 0; if (b.size() == 20 && !read32(b, at, raw)) return false; *reason = static_cast<NackReason>(raw); }
    return true;
}

fs::path framePath(const fs::path& root, uint64_t task, uint64_t seq) { return root / std::to_string(task) / (std::to_string(seq) + ".frame"); }
bool durableWrite(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec; fs::create_directories(path.parent_path(), ec); if (ec) return false;
    fs::path tmp = path; tmp += ".part";
#ifdef _WIN32
    FILE* file = nullptr; if (_wfopen_s(&file, tmp.c_str(), L"wb") != 0) return false;
#else
    FILE* file = fopen(tmp.c_str(), "wb"); if (!file) return false;
#endif
    bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && fflush(file) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(file)) == 0;
#else
    ok = ok && fsync(fileno(file)) == 0;
#endif
    ok = fclose(file) == 0 && ok;
    if (!ok) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, path, ec); if (ec) { fs::remove(tmp, ec); return false; } return true;
}
bool persistFrame(const fs::path& path, const Frame& f) {
    if (f.pixels.empty() || f.pixels.size() > kMaxFrameBytes) return false;
    auto b = metaBytes(f); b.insert(b.end(), f.pixels.begin(), f.pixels.end()); return durableWrite(path, b);
}
std::vector<fs::path> pendingPaths(const fs::path& root, uint64_t task) {
    std::vector<fs::path> out; std::error_code ec; fs::path dir = root / std::to_string(task);
    if (!fs::exists(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) if (e.path().extension() == ".frame") out.push_back(e.path());
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return std::stoull(a.stem().string()) < std::stoull(b.stem().string()); });
    return out;
}
uint64_t directoryBytes(const fs::path& root) {
    uint64_t total = 0; std::error_code ec; if (!fs::exists(root, ec)) return 0;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) if (e.is_regular_file(ec)) total += e.file_size(ec);
    return total;
}
uint64_t contiguousDurable(const fs::path& root, uint64_t task) {
    uint64_t seq = 0; for (const auto& p : pendingPaths(root, task)) { uint64_t n = std::stoull(p.stem().string()); if (n == seq + 1) seq = n; else if (n > seq + 1) break; } return seq;
}
fs::path completionPath(const fs::path& root, uint64_t task) { return root / "completed" / (std::to_string(task) + ".done"); }
bool hasCompletion(const fs::path& root, uint64_t task) { std::error_code ec; return fs::is_regular_file(completionPath(root, task), ec); }
uint64_t completedSeq(const fs::path& root, uint64_t task) {
    std::ifstream in(completionPath(root, task)); uint64_t seq = 0; if (in) in >> seq; return seq;
}
bool writeCompletion(const fs::path& root, uint64_t task, uint64_t seq) {
    const std::string text = std::to_string(seq) + "\n"; return durableWrite(completionPath(root, task), {text.begin(), text.end()});
}
} // namespace

Frame fromRawFrame(const RawFrame& raw, uint64_t taskId) {
    Frame f; f.taskId = taskId; f.frameSeq = raw.frameId; f.frameIndex = static_cast<uint32_t>(raw.frameId - 1);
    f.rows = raw.rows; f.cols = raw.cols; f.pixelType = raw.type; f.elemSize = raw.elemSize;
    if (!raw.empty()) f.pixels.assign(raw.data(), raw.data() + raw.totalBytes); return f;
}
bool loadFrame(const fs::path& path, Frame& f) {
    std::ifstream in(path, std::ios::binary | std::ios::ate); if (!in) return false;
    auto size = in.tellg(); if (size < 81 || size > static_cast<std::streamoff>(kMaxFrameBytes + 80)) return false;
    in.seekg(0); std::vector<uint8_t> b(static_cast<size_t>(size)); if (!in.read(reinterpret_cast<char*>(b.data()), size)) return false;
    std::vector<uint8_t> meta(b.begin(), b.begin() + 80); uint32_t chunks = 0, crc = 0;
    if (!parseMeta(meta, f, chunks, crc) || b.size() != 80 + f.pixels.size()) return false;
    std::copy(b.begin() + 80, b.end(), f.pixels.begin()); return crc32(f.pixels.data(), f.pixels.size()) == crc;
}

struct Sender::Impl {
    Impl(std::string h, uint16_t p, fs::path s, uint64_t t, SenderOptions o)
        : host(std::move(h)), port(p), spool(std::move(s)), task(t), options(o), spoolBytes(directoryBytes(spool / std::to_string(task))) {
        if (!task || !options.maxInFlightFrames || !options.maxQueuedBytes || options.maxSpoolBytes < kMaxFrameBytes + 80) {
            fatal = true; error = "invalid sender options";
        }
        for (const auto& path : pendingPaths(spool, task)) nextSeq = std::max(nextSeq, std::stoull(path.stem().string()) + 1);
    }
    ~Impl() { stopNow(); }

    std::string host; uint16_t port; fs::path spool; uint64_t task; SenderOptions options;
    Socket socket; mutable std::mutex mutex; std::condition_variable cv; std::thread worker;
    bool started = false, stopping = false, connected = false, connectedOnce = false;
    bool finishRequested = false, finished = false, fatal = false;
    uint64_t nextSeq = 1, spoolBytes = 0, connectionAttempts = 0, completedConnectAttempts = 0;
    std::string error; TransferStats stats;

    void ensureStartedLocked() {
        if (!started && !fatal) { started = true; worker = std::thread([this] { run(); }); }
    }
    void stopNow() {
        { std::lock_guard<std::mutex> lock(mutex); if (stopping) return; stopping = true; }
        socket.close(); cv.notify_all(); if (worker.joinable()) worker.join();
    }
    bool handshake() {
        socket.close();
        { std::lock_guard<std::mutex> lock(mutex); ++connectionAttempts; ++stats.connections; cv.notify_all(); }
        if (!connectTo(socket, host, port)) return false;
        std::vector<uint8_t> one; protocolv2::appendUint64(one, task); MessageType type{}; std::vector<uint8_t> body;
        if (!writeMessage(socket, MessageType::Hello, one) || !writeMessage(socket, MessageType::ResumeRequest, one) ||
            !readMessage(socket, type, body) || type != MessageType::ResumeReply) return false;
        uint64_t replyTask = 0, durable = 0; if (!parseReply(body, replyTask, durable) || replyTask != task) return false;
        uint64_t removed = 0;
        for (const auto& path : pendingPaths(spool, task)) {
            uint64_t seq = std::stoull(path.stem().string()); if (seq > durable) continue;
            std::error_code ec; uint64_t size = fs::file_size(path, ec); fs::remove(path, ec); if (!ec) removed += size;
        }
        { std::lock_guard<std::mutex> lock(mutex); spoolBytes = removed > spoolBytes ? 0 : spoolBytes - removed;
          nextSeq = std::max(nextSeq, durable + 1); connected = connectedOnce = true; error.clear(); cv.notify_all(); }
        return true;
    }
    bool transmit(const Frame& f) {
        if (!writeMessage(socket, MessageType::FrameBegin, metaBytes(f))) return false;
        for (size_t offset = 0, index = 0; offset < f.pixels.size(); offset += kChunkBytes, ++index) {
            uint32_t len = static_cast<uint32_t>(std::min<size_t>(kChunkBytes, f.pixels.size() - offset));
            std::vector<uint8_t> chunk; chunk.reserve(24 + len); protocolv2::appendUint64(chunk, f.frameSeq);
            protocolv2::appendUint32(chunk, static_cast<uint32_t>(index)); protocolv2::appendUint64(chunk, offset);
            protocolv2::appendUint32(chunk, len); chunk.insert(chunk.end(), f.pixels.begin() + offset, f.pixels.begin() + offset + len);
            if (!writeMessage(socket, MessageType::FrameChunk, chunk)) return false;
        }
        return true;
    }
    void disconnect(const std::string& why) {
        socket.close(); std::lock_guard<std::mutex> lock(mutex); connected = false; error = why; ++stats.reconnects; cv.notify_all();
    }
    void run() {
        std::map<uint64_t, uint64_t> inFlight;
        while (true) {
            { std::unique_lock<std::mutex> lock(mutex); if (stopping || fatal || finished) break; }
            if (!socket.valid() && !handshake()) {
                { std::lock_guard<std::mutex> lock(mutex); ++completedConnectAttempts; cv.notify_all(); }
                disconnect("connect or resume handshake failed");
                std::unique_lock<std::mutex> lock(mutex); cv.wait_for(lock, options.reconnectDelay, [this] { return stopping; }); continue;
            }
            auto paths = pendingPaths(spool, task); uint64_t queued = 0; for (const auto& pair : inFlight) queued += pair.second;
            bool sendFailed = false;
            for (const auto& path : paths) {
                if (inFlight.size() >= options.maxInFlightFrames) break;
                uint64_t seq = std::stoull(path.stem().string()); if (inFlight.count(seq)) continue;
                Frame f; if (!loadFrame(path, f)) { std::lock_guard<std::mutex> lock(mutex); fatal = true; error = "corrupt sender spool"; cv.notify_all(); sendFailed = true; break; }
                if (!inFlight.empty() && queued + f.pixels.size() > options.maxQueuedBytes) break;
                if (!transmit(f)) { sendFailed = true; break; }
                inFlight[seq] = f.pixels.size(); queued += f.pixels.size();
                { std::lock_guard<std::mutex> lock(mutex); stats.peakQueuedBytes = std::max(stats.peakQueuedBytes, queued); }
            }
            if (sendFailed) { inFlight.clear(); disconnect("frame transmission failed"); continue; }
            if (!inFlight.empty()) {
                MessageType type{}; std::vector<uint8_t> body; if (!readMessage(socket, type, body)) { inFlight.clear(); disconnect("ACK missing"); continue; }
                uint64_t replyTask = 0, seq = 0; NackReason reason{};
                if (!parseReply(body, replyTask, seq, &reason) || replyTask != task || (type != MessageType::Ack && type != MessageType::Nack)) {
                    inFlight.clear(); disconnect("invalid ACK/NACK"); continue;
                }
                if (type == MessageType::Nack) { inFlight.clear(); disconnect(reason == NackReason::StorageFull ? "receiver spool full" : "receiver NACK"); continue; }
                auto found = inFlight.find(seq); if (found == inFlight.end()) { inFlight.clear(); disconnect("ACK for unknown frame"); continue; }
                fs::path path = framePath(spool, task, seq); std::error_code ec; uint64_t disk = fs::file_size(path, ec); fs::remove(path, ec);
                if (ec) { std::lock_guard<std::mutex> lock(mutex); fatal = true; error = "cannot delete ACKed spool"; cv.notify_all(); break; }
                { std::lock_guard<std::mutex> lock(mutex); spoolBytes = disk > spoolBytes ? 0 : spoolBytes - disk;
                  ++stats.frames; stats.bytes += found->second; nextSeq = std::max(nextSeq, seq + 1); cv.notify_all(); }
                inFlight.erase(found); continue;
            }
            bool doFinish = false;
            { std::unique_lock<std::mutex> lock(mutex); doFinish = finishRequested;
              if (!doFinish) cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping || finishRequested || !pendingPaths(spool, task).empty(); }); }
            if (doFinish) {
                std::vector<uint8_t> body; protocolv2::appendUint64(body, task); MessageType type{}; std::vector<uint8_t> reply;
                uint64_t replyTask = 0, seq = 1; NackReason reason{};
                if (!writeMessage(socket, MessageType::TaskFinish, body) || !readMessage(socket, type, reply) ||
                    !parseReply(reply, replyTask, seq, &reason) || replyTask != task || seq != 0) {
                    disconnect("task finish ACK missing"); continue;
                }
                if (type == MessageType::Nack) { std::lock_guard<std::mutex> lock(mutex); fatal = true; error = "receiver failed to finalize task"; cv.notify_all(); break; }
                if (type != MessageType::Ack) { disconnect("invalid task finish reply"); continue; }
                std::lock_guard<std::mutex> lock(mutex); finished = true; error.clear(); cv.notify_all(); break;
            }
        }
    }
};

Sender::Sender(std::string host, uint16_t port, fs::path spoolDir, uint64_t taskId, SenderOptions options)
    : impl_(new Impl(std::move(host), port, std::move(spoolDir), taskId, options)) {}
Sender::~Sender() = default;
bool Sender::submit(const Frame& f) {
    if (!validFrame(f, impl_->task)) { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->error = "invalid frame"; return false; }
    const fs::path path = framePath(impl_->spool, impl_->task, f.frameSeq); const uint64_t bytes = 80 + f.pixels.size();
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (impl_->finishRequested || impl_->finished || impl_->fatal || impl_->stopping) { impl_->error = "sender is not accepting frames"; return false; }
    std::error_code ec;
    if (fs::exists(path, ec)) { Frame saved; if (!loadFrame(path, saved) || !sameFrame(saved, f)) { impl_->error = "frame sequence has different spooled data"; return false; } impl_->ensureStartedLocked(); return true; }
    impl_->cv.wait(lock, [&] { return impl_->stopping || impl_->fatal || impl_->spoolBytes + bytes <= impl_->options.maxSpoolBytes; });
    if (impl_->stopping || impl_->fatal) return false;
    if (!persistFrame(path, f)) { impl_->error = "cannot persist sender frame"; return false; }
    impl_->spoolBytes += fs::file_size(path, ec); impl_->nextSeq = std::max(impl_->nextSeq, f.frameSeq + 1);
    impl_->ensureStartedLocked(); lock.unlock(); impl_->cv.notify_all(); return true;
}
bool Sender::resume(unsigned maxAttempts) {
    std::unique_lock<std::mutex> lock(impl_->mutex); if (impl_->fatal) return false;
    uint64_t start = impl_->completedConnectAttempts; impl_->ensureStartedLocked(); impl_->cv.notify_all();
    impl_->cv.wait(lock, [&] { return impl_->connectedOnce || impl_->fatal || impl_->stopping || impl_->completedConnectAttempts >= start + std::max(1u, maxAttempts); });
    return impl_->connectedOnce;
}
bool Sender::send(const Frame& f, unsigned maxAttempts) {
    if (!submit(f)) return false; if (!resume(maxAttempts)) return false;
    std::unique_lock<std::mutex> lock(impl_->mutex); const fs::path path = framePath(impl_->spool, impl_->task, f.frameSeq);
    impl_->cv.wait(lock, [&] { std::error_code ec; return !fs::exists(path, ec) || impl_->fatal || impl_->stopping; });
    std::error_code ec; return !fs::exists(path, ec);
}
bool Sender::flush() {
    std::unique_lock<std::mutex> lock(impl_->mutex); impl_->ensureStartedLocked(); impl_->cv.notify_all();
    impl_->cv.wait(lock, [&] { return impl_->spoolBytes == 0 || impl_->fatal || impl_->stopping; }); return impl_->spoolBytes == 0;
}
bool Sender::finish(unsigned) {
    { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->fatal || impl_->stopping) return false; impl_->finishRequested = true; impl_->ensureStartedLocked(); }
    impl_->cv.notify_all(); std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->cv.wait(lock, [&] { return impl_->finished || impl_->fatal || impl_->stopping; }); return impl_->finished;
}
void Sender::stop() { impl_->stopNow(); }
size_t Sender::pendingCount() const { std::lock_guard<std::mutex> lock(impl_->mutex); return pendingPaths(impl_->spool, impl_->task).size(); }
uint64_t Sender::nextFrameSeq() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->nextSeq; }
TransferStats Sender::stats() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->stats; }
std::string Sender::lastError() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->error; }

struct Receiver::Impl {
    struct Item { Frame frame; };
    Impl(uint16_t p, fs::path s, ReceiverOptions o) : spool(std::move(s)), options(o), spoolBytes(directoryBytes(spool)) {
        if (!options.maxQueuedBytes || !options.maxSpoolBytes) { error = "invalid receiver options"; return; }
        listener.reset(listenOn(p)); if (!listener.valid()) { error = "listen failed"; return; }
        persistThread = std::thread([this] { persistLoop(); }); processThread = std::thread([this] { processLoop(); });
    }
    ~Impl() { stopNow(); }
    fs::path spool; ReceiverOptions options; Socket listener, client;
    mutable std::mutex mutex, sendMutex; std::condition_variable cv, spaceCv; std::thread persistThread, processThread;
    std::deque<Item> persistQueue; std::deque<fs::path> processQueue; uint64_t queuedBytes = 0, spoolBytes = 0;
    bool persistActive = false, processActive = false, stopping = false, finished = false, processingFailed = false;
    std::string error; FrameHandler handler; FinishHandler finishHandler; TransferStats stats;
    std::atomic<bool> dropAck{false}, dropFinishAck{false}, disconnectFrame{false};

    void stopNow() {
        { std::lock_guard<std::mutex> lock(mutex); if (stopping) return; stopping = true; }
        client.close(); listener.close(); cv.notify_all(); spaceCv.notify_all();
        if (persistThread.joinable()) persistThread.join(); if (processThread.joinable()) processThread.join();
    }
    bool sendSafe(MessageType type, const std::vector<uint8_t>& body) { std::lock_guard<std::mutex> lock(sendMutex); return writeMessage(client, type, body); }
    void failConnection(uint64_t task, uint64_t seq, NackReason reason, const std::string& message) {
        sendSafe(MessageType::Nack, nackBody(task, seq, reason));
        { std::lock_guard<std::mutex> lock(mutex); error = message; }
        client.close();
    }
    bool enqueue(Frame&& frame) {
        const uint64_t bytes = frame.pixels.size(); std::unique_lock<std::mutex> lock(mutex);
        spaceCv.wait(lock, [&] { return stopping || queuedBytes == 0 || queuedBytes + bytes <= options.maxQueuedBytes; });
        if (stopping) return false; queuedBytes += bytes; stats.peakQueuedBytes = std::max(stats.peakQueuedBytes, queuedBytes);
        persistQueue.push_back({std::move(frame)}); lock.unlock(); cv.notify_all(); return true;
    }
    void persistLoop() {
        for (;;) {
            Item item;
            { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return stopping || !persistQueue.empty(); });
              if (stopping && persistQueue.empty()) return; item = std::move(persistQueue.front()); persistQueue.pop_front(); persistActive = true; }
            Frame& f = item.frame; fs::path path = framePath(spool, f.taskId, f.frameSeq); bool ok = true, isNew = false; std::error_code ec;
            if (fs::exists(path, ec)) { Frame saved; ok = loadFrame(path, saved) && sameFrame(saved, f); }
            else {
                const uint64_t bytes = 80 + f.pixels.size();
                { std::lock_guard<std::mutex> lock(mutex); if (spoolBytes + bytes > options.maxSpoolBytes) ok = false; }
                if (ok) { ok = persistFrame(path, f); isNew = ok; }
                if (ok) { uint64_t disk = fs::file_size(path, ec); std::lock_guard<std::mutex> lock(mutex); spoolBytes += disk; }
            }
            if (!ok) failConnection(f.taskId, f.frameSeq, fs::exists(path, ec) ? NackReason::ProtocolError : NackReason::StorageFull,
                                    fs::exists(path, ec) ? "conflicting duplicate frame" : "receiver spool full or write failed");
            else {
                if (isNew) { std::lock_guard<std::mutex> lock(mutex); processQueue.push_back(path); cv.notify_all(); }
                if (dropAck.exchange(false)) client.close(); else if (!sendSafe(MessageType::Ack, two64(f.taskId, f.frameSeq))) client.close();
                std::lock_guard<std::mutex> lock(mutex); ++stats.frames; stats.bytes += f.pixels.size();
            }
            { std::lock_guard<std::mutex> lock(mutex); queuedBytes -= f.pixels.size(); persistActive = false; }
            cv.notify_all(); spaceCv.notify_all();
        }
    }
    void processLoop() {
        for (;;) {
            fs::path path;
            { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return stopping || !processQueue.empty(); });
              if (stopping && processQueue.empty()) return; path = processQueue.front(); processQueue.pop_front(); processActive = true; }
            try { Frame f; if (!loadFrame(path, f)) throw std::runtime_error("cannot reload receiver spool"); FrameHandler h; { std::lock_guard<std::mutex> lock(mutex); h = handler; } if (h) h(f); }
            catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mutex); processingFailed = true; error = e.what(); }
            catch (...) { std::lock_guard<std::mutex> lock(mutex); processingFailed = true; error = "frame handler failed"; }
            { std::lock_guard<std::mutex> lock(mutex); processActive = false; } cv.notify_all();
        }
    }
    bool waitPersistIdle() { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return stopping || (persistQueue.empty() && !persistActive); }); return !stopping; }
    bool waitAllIdle() { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return stopping || processingFailed || (persistQueue.empty() && !persistActive && processQueue.empty() && !processActive); }); return !stopping && !processingFailed; }
};

Receiver::Receiver(uint16_t port, fs::path spoolDir, ReceiverOptions options) : impl_(new Impl(port, std::move(spoolDir), options)) {}
Receiver::~Receiver() = default;
void Receiver::setFrameHandler(FrameHandler h) { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->handler = std::move(h); }
void Receiver::setFinishHandler(FinishHandler h) { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->finishHandler = std::move(h); }
void Receiver::dropNextAckForTest() { impl_->dropAck = true; }
void Receiver::dropNextFinishAckForTest() { impl_->dropFinishAck = true; }
void Receiver::disconnectNextFrameForTest() { impl_->disconnectFrame = true; }
void Receiver::stop() { impl_->stopNow(); }
TransferStats Receiver::stats() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->stats; }
std::string Receiver::lastError() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->error; }
bool Receiver::serveOne() {
    if (!impl_->listener.valid()) return false; SocketHandle s = ::accept(impl_->listener.get(), nullptr, nullptr); if (s == kBadSocket) return false;
    impl_->client.reset(s); { std::lock_guard<std::mutex> lock(impl_->mutex); ++impl_->stats.connections; }
    bool ok = handleClient(); impl_->waitPersistIdle(); impl_->client.close(); return ok;
}
bool Receiver::serveForever() { if (!impl_->listener.valid()) return false; while (true) { { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->stopping) break; } serveOne(); } return true; }
bool Receiver::serveUntilFinished() {
    if (!impl_->listener.valid()) return false;
    while (true) { { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->stopping || impl_->finished) return impl_->finished; } serveOne(); }
}
bool Receiver::handleClient() {
    MessageType type{}; std::vector<uint8_t> body; uint64_t task = 0; size_t at = 0;
    if (!readMessage(impl_->client, type, body) || type != MessageType::Hello || body.size() != 8 || !read64(body, at, task) || !task) return false;
    if (impl_->options.expectedTaskId && task != impl_->options.expectedTaskId) { impl_->failConnection(task, 0, NackReason::ProtocolError, "unexpected task id"); return false; }
    at = 0; uint64_t requested = 0;
    if (!readMessage(impl_->client, type, body) || type != MessageType::ResumeRequest || body.size() != 8 || !read64(body, at, requested) || requested != task) return false;
    const bool taskComplete = hasCompletion(impl_->spool, task);
    uint64_t complete = completedSeq(impl_->spool, task); uint64_t durable = taskComplete ? complete : contiguousDurable(impl_->spool, task);
    if (!impl_->sendSafe(MessageType::ResumeReply, two64(task, durable))) return false;
    while (readMessage(impl_->client, type, body)) {
        { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->stopping) return false; }
        if (type == MessageType::TaskFinish) {
            at = 0; uint64_t finishedTask = 0; if (body.size() != 8 || !read64(body, at, finishedTask) || finishedTask != task) return false;
            uint64_t seq = completedSeq(impl_->spool, task);
            if (!hasCompletion(impl_->spool, task)) {
                if (!impl_->waitAllIdle()) { impl_->sendSafe(MessageType::Nack, nackBody(task, 0, NackReason::ProcessingFailed)); return false; }
                FinishHandler handler; { std::lock_guard<std::mutex> lock(impl_->mutex); handler = impl_->finishHandler; }
                try { if (handler) handler(task); }
                catch (const std::exception& e) { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->error = e.what(); impl_->sendSafe(MessageType::Nack, nackBody(task, 0, NackReason::ProcessingFailed)); return false; }
                seq = contiguousDurable(impl_->spool, task); if (!writeCompletion(impl_->spool, task, seq)) return false;
            }
            if (impl_->options.cleanupCompletedTask) { std::error_code ec; fs::remove_all(impl_->spool / std::to_string(task), ec); if (ec) return false; }
            if (impl_->dropFinishAck.exchange(false)) { impl_->client.close(); return false; }
            if (!impl_->sendSafe(MessageType::Ack, two64(task, 0))) return false;
            { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->finished = true; } impl_->cv.notify_all(); return true;
        }
        if (type != MessageType::FrameBegin || taskComplete) return false;
        Frame f; uint32_t chunks = 0, expectedCrc = 0; if (!parseMeta(body, f, chunks, expectedCrc) || f.taskId != task) return false;
        if (impl_->disconnectFrame.exchange(false)) return false;
        for (uint32_t i = 0; i < chunks; ++i) {
            if (!readMessage(impl_->client, type, body) || type != MessageType::FrameChunk) return false;
            size_t pos = 0; uint64_t seq = 0, offset = 0; uint32_t index = 0, len = 0;
            if (!read64(body, pos, seq) || !read32(body, pos, index) || !read64(body, pos, offset) || !read32(body, pos, len) ||
                seq != f.frameSeq || index != i || offset != uint64_t(i) * kChunkBytes || !len || len > kChunkBytes ||
                offset + len > f.pixels.size() || pos + len != body.size()) return false;
            std::copy(body.begin() + pos, body.end(), f.pixels.begin() + offset);
        }
        if (crc32(f.pixels.data(), f.pixels.size()) != expectedCrc) { impl_->sendSafe(MessageType::Nack, nackBody(task, f.frameSeq, NackReason::CrcMismatch)); continue; }
        if (!impl_->enqueue(std::move(f))) return false;
    }
    return false;
}
} // namespace v2transfer
