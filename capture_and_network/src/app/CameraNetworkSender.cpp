#include "CameraNetworkSender.h"

#include <iostream>

CameraNetworkSender::CameraNetworkSender(
    int cameraIndex,
    const std::string& serverIp,
    int serverPort
)
    : queue_(10),
      bufferPool_(1920 * 1080 * 3, 16),
      camera_(cameraIndex, queue_, bufferPool_),
      tcpClient_(serverIp, serverPort),
      frameSender_(tcpClient_),
      running_(false) {}

CameraNetworkSender::~CameraNetworkSender() {
    stop();
}

void CameraNetworkSender::setCameraConfig(int width, int height, int fps) {
    camera_.setConfig(width, height, fps);
}

void CameraNetworkSender::setFragmentSize(uint32_t fragmentSize) {
    frameSender_.setFragmentSize(fragmentSize);
}

bool CameraNetworkSender::start() {
    if (running_) {
        return true;
    }

    if (!tcpClient_.connectToServer()) {
        return false;
    }

    running_ = true;

    if (!camera_.start()) {
        running_ = false;
        tcpClient_.closeConnection();
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

    if (senderThread_.joinable()) {
        senderThread_.join();
    }

    tcpClient_.closeConnection();

    std::cout << "[CameraNetworkSender] 已停止" << std::endl;
}

void CameraNetworkSender::sendLoop() {
    std::cout << "[CameraNetworkSender] 发送线程启动" << std::endl;

    while (running_) {
        RawFrame frame;

        if (!queue_.pop(frame)) {
            break;
        }

        if (!tcpClient_.isConnected()) {
            std::cerr << "[CameraNetworkSender] TCP 连接已断开" << std::endl;
            break;
        }

        if (!frameSender_.sendFrame(frame)) {
            std::cerr << "[CameraNetworkSender] 发送原始帧失败" << std::endl;
            break;
        }
    }

    std::cout << "[CameraNetworkSender] 发送线程退出" << std::endl;
}