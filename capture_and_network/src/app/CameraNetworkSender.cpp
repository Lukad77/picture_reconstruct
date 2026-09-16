#include "CameraNetworkSender.h"

#include <iostream>
#include <chrono>

CameraNetworkSender::CameraNetworkSender(
    int cameraIndex,
    const std::string& serverIp,
    int serverPort
)
    : queue_(10),
      bufferPool_(1920 * 1080 * 3, 16),
      camera_(cameraIndex, queue_, bufferPool_),
      frameSender_(serverIp, static_cast<uint16_t>(serverPort), "sender_spool", 1),
      running_(false) {}

CameraNetworkSender::~CameraNetworkSender() {
    stop();
}

void CameraNetworkSender::setCameraConfig(int width, int height, int fps) {
    camera_.setConfig(width, height, fps);
}

void CameraNetworkSender::setFragmentSize(uint32_t fragmentSize) {
    (void)fragmentSize; // Protocol V2 uses the fixed 64 KiB chunk size.
}

bool CameraNetworkSender::start() {
    if (running_) {
        return true;
    }

    if (!frameSender_.resume()) {
        std::cerr << "[CameraNetworkSender] V2 resume failed: "
                  << frameSender_.lastError() << std::endl;
        return false;
    }

    running_ = true;

    if (!camera_.start()) {
        running_ = false;
        return false;
    }

    senderThread_ = std::thread(&CameraNetworkSender::sendLoop, this);

    std::cout << "[CameraNetworkSender] 启动成功" << std::endl;

    return true;
}

void CameraNetworkSender::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    camera_.stop();
    queue_.stop();
    frameSender_.stop();

    if (senderThread_.joinable()) {
        senderThread_.join();
    }

    std::cout << "[CameraNetworkSender] 已停止" << std::endl;
}

void CameraNetworkSender::sendLoop() {
    std::cout << "[CameraNetworkSender] 发送线程启动" << std::endl;

    while (running_) {
        RawFrame frame;

        if (!queue_.pop(frame)) {
            break;
        }

        auto outgoing = v2transfer::fromRawFrame(frame, 1);
        outgoing.frameSeq = frameSender_.nextFrameSeq();
        outgoing.frameIndex = static_cast<uint32_t>(outgoing.frameSeq - 1);
        while (running_ && !frameSender_.submit(outgoing)) {
            std::cerr << "[CameraNetworkSender] V2 提交失败，帧保留在本地或等待恢复: "
                      << frameSender_.lastError() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    std::cout << "[CameraNetworkSender] 发送线程退出" << std::endl;
}
