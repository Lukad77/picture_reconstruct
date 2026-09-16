#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RawFrame.h"

namespace v2transfer {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

struct Frame {
    uint64_t taskId = 1;
    uint32_t lineId = 0;
    uint32_t attemptId = 1;
    uint32_t frameIndex = 0;
    uint64_t frameSeq = 0;
    double stageX = 0.0;
    double stageY = 0.0;
    uint32_t rows = 0;
    uint32_t cols = 0;
    uint32_t pixelType = 0;
    uint32_t elemSize = 0;
    std::vector<uint8_t> pixels;
};

struct SenderOptions {
    size_t maxInFlightFrames = 4;
    uint64_t maxQueuedBytes = 64 * kMiB;
    uint64_t maxSpoolBytes = 20 * kGiB;
    std::chrono::milliseconds reconnectDelay{150};
};

struct ReceiverOptions {
    uint64_t expectedTaskId = 0;
    uint64_t maxQueuedBytes = 64 * kMiB;
    uint64_t maxSpoolBytes = 20 * kGiB;
    bool cleanupCompletedTask = true;
};

struct TransferStats {
    uint64_t connections = 0;
    uint64_t reconnects = 0;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    uint64_t peakQueuedBytes = 0;
};

enum class NackReason : uint32_t {
    Unspecified = 0,
    CrcMismatch = 1,
    StorageFull = 2,
    ProcessingFailed = 3,
    ProtocolError = 4
};

Frame fromRawFrame(const RawFrame& raw, uint64_t taskId = 1);

class Sender {
public:
    Sender(std::string host, uint16_t port, std::filesystem::path spoolDir,
           uint64_t taskId = 1, SenderOptions options = {});
    ~Sender();
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    bool submit(const Frame& frame);
    bool send(const Frame& frame, unsigned maxAttempts = 5);
    bool resume(unsigned maxAttempts = 5);
    bool flush();
    bool finish(unsigned maxAttempts = 5);
    void stop();

    size_t pendingCount() const;
    uint64_t nextFrameSeq() const;
    TransferStats stats() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Receiver {
public:
    using FrameHandler = std::function<void(const Frame&)>;
    using FinishHandler = std::function<void(uint64_t)>;

    Receiver(uint16_t port, std::filesystem::path spoolDir, ReceiverOptions options = {});
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    bool serveOne();
    bool serveForever();
    bool serveUntilFinished();
    void stop();
    void setFrameHandler(FrameHandler handler);
    void setFinishHandler(FinishHandler handler);
    void dropNextAckForTest();
    void dropNextFinishAckForTest();
    void disconnectNextFrameForTest();
    TransferStats stats() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool handleClient();
};

bool loadFrame(const std::filesystem::path& path, Frame& frame);

} // namespace v2transfer
