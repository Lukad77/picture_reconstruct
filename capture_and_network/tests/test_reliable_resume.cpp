#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "ProtocolV2.h"
#include "RawFrame.h"
#include "RawFrameProtocol.h"
#include "ResumeSession.h"

namespace {

bool recvExact(int sock, void* buf, size_t size) {
    auto* ptr = static_cast<uint8_t*>(buf);
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = recv(sock, ptr + offset, size - offset, 0);
        if (n <= 0) {
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

void serverLoop(int port, std::atomic<bool>& ready, std::atomic<bool>& gotReplay) {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        return;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(serverFd);
        return;
    }

    if (listen(serverFd, 1) < 0) {
        close(serverFd);
        return;
    }

    ready.store(true, std::memory_order_release);
    int clientFd = accept(serverFd, nullptr, nullptr);
    if (clientFd < 0) {
        close(serverFd);
        return;
    }

    std::vector<uint8_t> helloMessage(20);
    if (!recvExact(clientFd, helloMessage.data(), helloMessage.size())) {
        std::cerr << "server: failed to read hello message\n";
        close(clientFd);
        close(serverFd);
        return;
    }

    std::vector<uint8_t> resumeRequest(20);
    if (!recvExact(clientFd, resumeRequest.data(), resumeRequest.size())) {
        std::cerr << "server: failed to read resume request\n";
        close(clientFd);
        close(serverFd);
        return;
    }

    std::vector<uint8_t> resumeReply = protocolv2::makeResumeReplyMessage(1, 0);
    if (send(clientFd, resumeReply.data(), resumeReply.size(), 0) < 0) {
        std::cerr << "server: failed to send resume reply\n";
        close(clientFd);
        close(serverFd);
        return;
    }

    std::vector<uint8_t> frameHeaderBytes(RawFrameProtocol::FRAME_HEADER_SIZE);
    if (!recvExact(clientFd, frameHeaderBytes.data(), frameHeaderBytes.size())) {
        std::cerr << "server: failed to read frame header\n";
        close(clientFd);
        close(serverFd);
        return;
    }

    RawFrameHeader frameHeader{};
    if (!RawFrameProtocol::parseFrameHeader(frameHeaderBytes.data(), frameHeaderBytes.size(), frameHeader)) {
        std::cerr << "server: failed to parse frame header\n";
        close(clientFd);
        close(serverFd);
        return;
    }

    std::vector<uint8_t> rawBuffer(frameHeader.totalBytes);
    uint32_t totalReceived = 0;
    for (uint32_t i = 0; i < frameHeader.fragmentCount; ++i) {
        std::vector<uint8_t> chunkHeaderBytes(RawFrameProtocol::CHUNK_HEADER_SIZE);
        if (!recvExact(clientFd, chunkHeaderBytes.data(), chunkHeaderBytes.size())) {
            std::cerr << "server: failed to read chunk header\n";
            close(clientFd);
            close(serverFd);
            return;
        }

        RawChunkHeader chunkHeader{};
        if (!RawFrameProtocol::parseChunkHeader(chunkHeaderBytes.data(), chunkHeaderBytes.size(), chunkHeader)) {
            std::cerr << "server: failed to parse chunk header\n";
            close(clientFd);
            close(serverFd);
            return;
        }

        if (!recvExact(clientFd, rawBuffer.data() + chunkHeader.offset, chunkHeader.payloadSize)) {
            std::cerr << "server: failed to read frame payload offset=" << chunkHeader.offset
                      << " size=" << chunkHeader.payloadSize << "\n";
            close(clientFd);
            close(serverFd);
            return;
        }
        totalReceived += chunkHeader.payloadSize;
    }

    std::cerr << "server: totalReceived=" << totalReceived << ", expected=" << frameHeader.totalBytes << "\n";
    gotReplay.store(totalReceived == frameHeader.totalBytes, std::memory_order_release);

    close(clientFd);
    close(serverFd);
}

} // namespace

int main() {
    const int port = 19002;
    const std::string spoolRoot = "/tmp/reliable_resume_test_spool";

    std::filesystem::remove_all(spoolRoot);
    std::atomic<bool> ready{false};
    std::atomic<bool> gotReplay{false};

    std::thread server(serverLoop, port, std::ref(ready), std::ref(gotReplay));

    auto buffer = std::shared_ptr<uint8_t[]>(new uint8_t[16]);
    for (int i = 0; i < 16; ++i) {
        buffer[i] = static_cast<uint8_t>(i + 5);
    }

    RawFrame frame(42, 4, 4, CV_8UC1, 1, 16, buffer);
    ResumeSession session("127.0.0.1", port, spoolRoot, 1);
    session.spool().persist(frame);

    for (int attempt = 0; attempt < 100 && !ready.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!session.start()) {
        std::cerr << "resume session failed" << std::endl;
        server.join();
        std::filesystem::remove_all(spoolRoot);
        return 1;
    }

    if (session.state() != SessionState::Ready) {
        std::cerr << "session not ready" << std::endl;
        server.join();
        std::filesystem::remove_all(spoolRoot);
        return 2;
    }

    server.join();

    if (!gotReplay.load()) {
        std::cerr << "pending frame was not replayed" << std::endl;
        std::filesystem::remove_all(spoolRoot);
        return 3;
    }

    std::cout << "resume state machine ok" << std::endl;
    std::filesystem::remove_all(spoolRoot);
    return 0;
}
