#pragma once

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RawFrame.h"

namespace v2transfer {

struct Frame {
    // 一个任务内所有帧共享 taskId；receiver 用它隔离 spool 目录。
    uint64_t taskId = 1;
    // line/attempt 为扫描任务恢复预留的逻辑层级。
    uint32_t lineId = 0;
    uint32_t attemptId = 1;
    uint32_t frameIndex = 0;
    // frameSeq 是可靠传输序号，也是 spool 文件名的一部分。
    uint64_t frameSeq = 0;
    double stageX = 0.0;
    double stageY = 0.0;
    uint32_t rows = 0;
    uint32_t cols = 0;
    uint32_t pixelType = 0;
    uint32_t elemSize = 0;
    // 原始像素，不经过 JPEG/PNG 编解码。
    std::vector<uint8_t> pixels;
};

Frame fromRawFrame(const RawFrame& raw, uint64_t taskId = 1);

class Sender {
public:
    Sender(std::string host, uint16_t port, std::filesystem::path spoolDir, uint64_t taskId = 1);
    ~Sender();
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    // 先持久化，再连接、重放 pending 帧并等待对应 ACK。
    bool send(const Frame& frame, unsigned maxAttempts = 5);
    // Hello + ResumeRequest + pending frame replay。
    bool resume(unsigned maxAttempts = 5);
    // 所有帧 ACK 后提交任务结束消息。
    bool finish(unsigned maxAttempts = 5);
    size_t pendingCount() const;
    uint64_t nextFrameSeq() const { return nextFrameSeq_; }
    const std::string& lastError() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path spoolDir_;
    std::string host_;
    uint16_t port_;
    uint64_t taskId_;
    uint64_t nextFrameSeq_ = 1;
    std::string error_;
    bool persist(const Frame& frame);
    bool attemptResume();
};

class Receiver {
public:
    using FrameHandler = std::function<void(const Frame&)>;
    using FinishHandler = std::function<void(uint64_t)>;
    Receiver(uint16_t port, std::filesystem::path spoolDir);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    // 处理一个连接；断开后可再次 accept。
    bool serveOne();
    bool serveForever();
    // 处理连接直到收到合法 TaskFinish。
    bool serveUntilFinished();
    void stop();
    void setFrameHandler(FrameHandler handler);
    void setFinishHandler(FinishHandler handler);
    void dropNextAckForTest();
    void disconnectNextFrameForTest();
    const std::string& lastError() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path spoolDir_;
    std::string error_;
    FrameHandler handler_;
    FinishHandler finishHandler_;
    std::atomic<bool> dropAck_{false};
    std::atomic<bool> disconnectFrame_{false};
    bool handleClient();
};

bool loadFrame(const std::filesystem::path& path, Frame& frame);

} // namespace v2transfer
