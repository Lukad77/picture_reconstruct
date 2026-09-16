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
// 单帧内存上限：防止恶意 dataLength 触发无限制分配。
constexpr uint64_t kMaxFrameBytes = 200ULL * 1024 * 1024;
// 原始图像按 64 KiB 切片，和协议约定保持一致。
constexpr uint32_t kChunkBytes = 64 * 1024;
// FrameChunk 的 body = 24 字节描述头 + 一个最大 chunk。
constexpr uint32_t kMaxMessageBytes = kChunkBytes + 24;

#ifdef _WIN32
// Windows 使用 SOCKET，Linux 使用 int；其余代码通过别名保持一致。
using SocketHandle = SOCKET;
constexpr SocketHandle kBadSocket = INVALID_SOCKET;
// 释放 Windows socket 句柄。
void closeSocket(SocketHandle s) { if (s != kBadSocket) closesocket(s); }
bool initSockets() {
    // WSAStartup 只执行一次，避免每个连接重复初始化 Winsock。
    static bool ok = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    return ok;
}
#else
// Linux socket 是文件描述符。
using SocketHandle = int;
constexpr SocketHandle kBadSocket = -1;
// 关闭 Linux 文件描述符。
void closeSocket(SocketHandle s) { if (s != kBadSocket) ::close(s); }
bool initSockets() { return true; }
#endif

class Socket {
public:
    Socket() = default;
    explicit Socket(SocketHandle s) : handle_(s) { setTimeout(); }
    // RAII 保证异常或提前返回时不会泄漏连接。
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    bool valid() const { return handle_ != kBadSocket; }
    SocketHandle get() const { return handle_; }
    void reset(SocketHandle s) { close(); handle_ = s; setTimeout(); }
    void close() { closeSocket(handle_); handle_ = kBadSocket; }
    // TCP 可能只发送部分字节，因此必须循环到 length 变成 0。
    bool sendAll(const uint8_t* data, size_t length) {
        while (length) {
            int n = ::send(handle_, reinterpret_cast<const char*>(data),
                           static_cast<int>(std::min<size_t>(length, 1 << 20)), 0);
            if (n <= 0) return false;
            data += n; length -= static_cast<size_t>(n);
        }
        return true;
    }
    // TCP 没有消息边界；该函数负责读取一个完整协议字段或 body。
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
        // 超时避免对端连接后不发送数据导致线程永久阻塞。
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
    // 每次 Resume 都创建一个全新的 TCP 连接。
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
    // 监听所有网卡，支持 sender 和 receiver 位于不同 Linux 主机。
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
    // 所有整数都按大端读取，并在读取前检查剩余长度。
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
    // CRC 覆盖整帧像素，而不是单独覆盖某一个 chunk。
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

std::vector<uint8_t> metaBytes(const Frame& f) {
    // 序列化 FrameBegin body；字段顺序必须与 parseMeta 完全一致。
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
    // 固定 80 字节可以阻止字段错位和未知版本数据被误解释。
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
    // 只有通过长度检查后才根据 dataLength 分配帧缓冲区。
    f.pixels.resize(static_cast<size_t>(len));
    return true;
}

bool writeMessage(Socket& socket, MessageType type, const std::vector<uint8_t>& body) {
    // 先写 12 字节公共头，再写 body；接收端据此恢复消息边界。
    auto header = protocolv2::makeMessageHeader(type, static_cast<uint32_t>(body.size()));
    return socket.sendAll(header.data(), header.size()) &&
           (body.empty() || socket.sendAll(body.data(), body.size()));
}
bool readMessage(Socket& socket, MessageType& type, std::vector<uint8_t>& body) {
    // 先精确读取公共头，再依据 bodyLength 精确读取 body，兼容半包和粘包。
    std::vector<uint8_t> header(12);
    if (!socket.recvAll(header.data(), header.size())) return false;
    size_t at = 0; uint32_t magic = 0, length = 0; uint16_t version = 0, rawType = 0;
    if (!read32(header, at, magic) || !read16(header, at, version) ||
        !read16(header, at, rawType) || !read32(header, at, length) ||
        // 未知 magic/version 或超长 body 立即拒绝，避免内存耗尽和协议错位。
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
    // 先写旁路文件，再原子 rename；正式 .frame 不会暴露半写入内容。
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
    // fwrite 写全量、fflush 刷入 libc，再由 fsync/_commit 刷入操作系统。
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
    // 接收端和发送端 spool 都使用同一套完整帧文件格式。
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
    // 只报告从序号 1 开始连续存在的帧，避免跳帧被误认为 durable。
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
    // 从 spool 恢复时重新校验元数据、文件长度和 CRC。
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
    // persist-first：任何网络操作之前先把完整帧写入本地磁盘。
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
    // Resume 是“新连接 + 握手 + 重放待确认帧”的完整恢复事务。
    impl_->socket.close();
    if (!connectTo(impl_->socket, host_, port_)) { error_ = "connect failed"; return false; }
    std::vector<uint8_t> one; protocolv2::appendUint64(one, taskId_);
    MessageType type{}; std::vector<uint8_t> body;
    // Hello 和 ResumeRequest 都携带 taskId，防止跨任务复用连接状态。
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
    // 远端已经 durable 的帧直接清理，本地仍 pending 的帧按序整帧重发。
    for (const auto& path : pendingPaths(spoolDir_, taskId_)) {
        Frame f;
        if (!loadFrame(path, f)) { error_ = "corrupt sender spool"; return false; }
        if (f.frameSeq <= durable) { std::error_code ec; fs::remove(path, ec); if (ec) return false; continue; }
        auto meta = metaBytes(f);
        if (!writeMessage(impl_->socket, MessageType::FrameBegin, meta)) {
            error_ = "frame begin failed"; return false;
        }
        // 每次重连都从 FrameBegin 开始，不尝试跨连接续传半个 chunk。
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
        // 只有收到对应 frameSeq 的 ACK 才删除 sender spool。
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
    // 所有帧 ACK 完成后发送 TaskFinish，通知 receiver 可以提交重构结果。
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
    // 一次处理一个 TCP client；断线后回到 accept 等待重连。
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
    // Linux 重构入口使用该模式，在收到合法 TaskFinish 后退出。
    if (!impl_->listener.valid()) return false;
    while (!impl_->stopped.load() && !impl_->finished.load()) {
        serveOne();
    }
    return impl_->finished.load();
}

bool Receiver::handleClient() {
    // 每条新连接都必须重新完成 Hello/Resume，旧连接状态不会沿用。
    MessageType type{}; std::vector<uint8_t> body; uint64_t task = 0;
    // 第一个消息必须是 Hello，且 body 只能包含一个 uint64 taskId。
    if (!readMessage(impl_->client, type, body) || type != MessageType::Hello || body.size() != 8) return false;
    size_t at = 0; if (!read64(body, at, task) || !task) return false;
    if (!readMessage(impl_->client, type, body) || type != MessageType::ResumeRequest || body.size() != 8) return false;
    at = 0; uint64_t requested = 0;
    if (!read64(body, at, requested) || requested != task) return false;
    // 返回远端连续 durable 的最高序号，让 sender 决定哪些帧需要重放。
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
        // FrameBegin 创建本次接收的内存组装区，但尚未写入正式文件。
        Frame f; uint32_t chunks = 0, expectedCrc = 0;
        if (!parseMeta(body, f, chunks, expectedCrc) || f.taskId != task) return false;
        fs::path path = framePath(spoolDir_, task, f.frameSeq);
        if (disconnectFrame_.exchange(false)) return false;
        // 严格按 chunk index/offset 接收，乱序或越界数据直接终止连接。
        for (uint32_t i = 0; i < chunks; ++i) {
            if (!readMessage(impl_->client, type, body) || type != MessageType::FrameChunk) return false;
            size_t pos = 0; uint64_t seq = 0, offset = 0; uint32_t index = 0, len = 0;
            if (!read64(body, pos, seq) || !read32(body, pos, index) ||
                !read64(body, pos, offset) || !read32(body, pos, len) ||
                seq != f.frameSeq || index != i || offset != uint64_t(i) * kChunkBytes ||
                !len || len > kChunkBytes || offset + len > f.pixels.size() || pos + len != body.size()) return false;
            std::copy(body.begin() + pos, body.end(), f.pixels.begin() + offset);
        }
        // CRC 失败只返回 NACK，不落盘、不调用业务处理器。
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
            // 新帧先 durable 落盘，再交给重构回调。
            if (!persistFrame(path, f)) { error_ = "receiver durable write failed"; return false; }
            if (handler_) handler_(f);
        }
        if (dropAck_.exchange(false)) return false;
        // ACK 是业务确认：表示文件已完整写入且业务处理已接受。
        if (!writeMessage(impl_->client, MessageType::Ack, two64(task, f.frameSeq))) return false;
    }
    return true;
}
} // namespace v2transfer
