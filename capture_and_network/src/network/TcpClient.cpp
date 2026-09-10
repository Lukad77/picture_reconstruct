#include "TcpClient.h"

#include <algorithm>
#include <iostream>
#include <cstring>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

TcpClient::TcpClient(const std::string& ip, int port)
    : ip_(ip),
      port_(port),
      socketFd_(-1),
      connected_(false),
      sendTimeoutSeconds_(10),
      recvTimeoutSeconds_(10) {}

TcpClient::~TcpClient() {
    closeConnection();
}

bool TcpClient::connectToServer() {
    if (connected_) {
        return true;
    }

    socketFd_ = socket(AF_INET, SOCK_STREAM, 0);

    if (socketFd_ < 0) {
        std::cerr << "[TcpClient] 创建 socket 失败" << std::endl;
        return false;
    }

    timeval sendTimeout{};
    sendTimeout.tv_sec = sendTimeoutSeconds_;
    sendTimeout.tv_usec = 0;
    setsockopt(socketFd_, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

    timeval recvTimeout{};
    recvTimeout.tv_sec = recvTimeoutSeconds_;
    recvTimeout.tv_usec = 0;
    setsockopt(socketFd_, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port_);

    if (inet_pton(AF_INET, ip_.c_str(), &serverAddr.sin_addr) <= 0) {
        std::cerr << "[TcpClient] IP 地址无效: " << ip_ << std::endl;
        ::close(socketFd_);
        socketFd_ = -1;
        return false;
    }

    if (connect(socketFd_, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        std::cerr << "[TcpClient] 连接服务器失败: " << ip_ << ":" << port_ << std::endl;
        ::close(socketFd_);
        socketFd_ = -1;
        return false;
    }

    connected_ = true;

    std::cout << "[TcpClient] 已连接服务器: " << ip_ << ":" << port_ << std::endl;

    return true;
}

void TcpClient::closeConnection() {
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }

    connected_ = false;
}

bool TcpClient::isConnected() const {
    return connected_;
}

bool TcpClient::sendAll(const void* data, size_t size) {
    if (!connected_) {
        return false;
    }

    const uint8_t* buffer = static_cast<const uint8_t*>(data);
    size_t sentBytes = 0;

    while (sentBytes < size) {
        ssize_t n = send(socketFd_, buffer + sentBytes, size - sentBytes, 0);

        if (n <= 0) {
            std::cerr << "[TcpClient] send 失败" << std::endl;
            closeConnection();
            return false;
        }

        sentBytes += static_cast<size_t>(n);
    }

    return true;
}

bool TcpClient::recvAll(void* data, size_t size) {
    if (!connected_) {
        return false;
    }

    uint8_t* buffer = static_cast<uint8_t*>(data);
    size_t receivedBytes = 0;

    while (receivedBytes < size) {
        ssize_t n = recv(socketFd_, buffer + receivedBytes, size - receivedBytes, 0);

        if (n <= 0) {
            std::cerr << "[TcpClient] recv 失败或连接已断开" << std::endl;
            closeConnection();
            return false;
        }

        receivedBytes += static_cast<size_t>(n);
    }

    return true;
}

void TcpClient::setSendTimeout(int seconds) {
    sendTimeoutSeconds_ = std::max(1, seconds);
}

void TcpClient::setRecvTimeout(int seconds) {
    recvTimeoutSeconds_ = std::max(1, seconds);
}