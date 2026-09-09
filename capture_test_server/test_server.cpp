#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>

#include <opencv2/opencv.hpp>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "RawFrameProtocol.h"

bool recvAll(int socketFd, void* data, size_t size) {
    uint8_t* buffer = static_cast<uint8_t*>(data);
    size_t receivedBytes = 0;

    while (receivedBytes < size) {
        ssize_t n = recv(socketFd, buffer + receivedBytes, size - receivedBytes, 0);

        if (n <= 0) {
            return false;
        }

        receivedBytes += static_cast<size_t>(n);
    }

    return true;
}

bool saveRawFile(
    const std::string& filename,
    const std::vector<uint8_t>& buffer
) {
    std::ofstream file(filename, std::ios::binary);

    if (!file.is_open()) {
        return false;
    }

    file.write(
        reinterpret_cast<const char*>(buffer.data()),
        static_cast<std::streamsize>(buffer.size())
    );

    return true;
}

bool saveAsPpmForView(
    const std::string& filename,
    const cv::Mat& bgrImage
) {
    if (bgrImage.empty()) {
        return false;
    }

    if (bgrImage.type() != CV_8UC3) {
        std::cerr << "当前图像不是 CV_8UC3，无法保存为 PPM" << std::endl;
        return false;
    }

    std::ofstream file(filename, std::ios::binary);

    if (!file.is_open()) {
        return false;
    }

    file << "P6\n";
    file << bgrImage.cols << " " << bgrImage.rows << "\n";
    file << "255\n";

    for (int r = 0; r < bgrImage.rows; ++r) {
        const cv::Vec3b* row = bgrImage.ptr<cv::Vec3b>(r);

        for (int c = 0; c < bgrImage.cols; ++c) {
            uint8_t b = row[c][0];
            uint8_t g = row[c][1];
            uint8_t rValue = row[c][2];

            file.put(static_cast<char>(rValue));
            file.put(static_cast<char>(g));
            file.put(static_cast<char>(b));
        }
    }

    return true;
}

int main() {
    int port = 9000;

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket < 0) {
        std::cerr << "创建 socket 失败" << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        std::cerr << "bind 失败" << std::endl;
        close(serverSocket);
        return 1;
    }

    if (listen(serverSocket, 5) < 0) {
        std::cerr << "listen 失败" << std::endl;
        close(serverSocket);
        return 1;
    }

    std::cout << "服务器监听端口: " << port << std::endl;

    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);

    int clientSocket = accept(
        serverSocket,
        reinterpret_cast<sockaddr*>(&clientAddr),
        &clientLen
    );

    if (clientSocket < 0) {
        std::cerr << "accept 失败" << std::endl;
        close(serverSocket);
        return 1;
    }

    std::cout << "客户端已连接" << std::endl;

    while (true) {
        std::vector<uint8_t> frameHeaderBytes(RawFrameProtocol::FRAME_HEADER_SIZE);

        if (!recvAll(clientSocket, frameHeaderBytes.data(), frameHeaderBytes.size())) {
            std::cout << "客户端断开连接" << std::endl;
            break;
        }

        RawFrameHeader frameHeader{};

        if (!RawFrameProtocol::parseFrameHeader(
                frameHeaderBytes.data(),
                frameHeaderBytes.size(),
                frameHeader
            )) {
            std::cerr << "解析帧头失败" << std::endl;
            break;
        }

        if (frameHeader.totalBytes == 0 ||
            frameHeader.totalBytes > 200ULL * 1024ULL * 1024ULL) {
            std::cerr << "非法帧大小: " << frameHeader.totalBytes << std::endl;
            break;
        }

        std::vector<uint8_t> rawBuffer(frameHeader.totalBytes);

        for (uint32_t i = 0; i < frameHeader.fragmentCount; ++i) {
            std::vector<uint8_t> chunkHeaderBytes(RawFrameProtocol::CHUNK_HEADER_SIZE);

            if (!recvAll(clientSocket, chunkHeaderBytes.data(), chunkHeaderBytes.size())) {
                std::cerr << "接收分片头失败" << std::endl;
                close(clientSocket);
                close(serverSocket);
                return 1;
            }

            RawChunkHeader chunkHeader{};

            if (!RawFrameProtocol::parseChunkHeader(
                    chunkHeaderBytes.data(),
                    chunkHeaderBytes.size(),
                    chunkHeader
                )) {
                std::cerr << "解析分片头失败" << std::endl;
                close(clientSocket);
                close(serverSocket);
                return 1;
            }

            if (chunkHeader.frameId != frameHeader.frameId) {
                std::cerr << "分片 frameId 不匹配" << std::endl;
                break;
            }

            if (chunkHeader.offset + chunkHeader.payloadSize > frameHeader.totalBytes) {
                std::cerr << "分片越界" << std::endl;
                break;
            }

            if (!recvAll(
                    clientSocket,
                    rawBuffer.data() + chunkHeader.offset,
                    chunkHeader.payloadSize
                )) {
                std::cerr << "接收分片数据失败" << std::endl;
                close(clientSocket);
                close(serverSocket);
                return 1;
            }
        }

        std::cout << "收到原始帧 frameId="
                  << frameHeader.frameId
                  << ", size=" << frameHeader.totalBytes
                  << ", rows=" << frameHeader.rows
                  << ", cols=" << frameHeader.cols
                  << ", type=" << frameHeader.type
                  << std::endl;

        std::string rawFilename =
            "frame_" + std::to_string(frameHeader.frameId) + ".raw";

        saveRawFile(rawFilename, rawBuffer);

        cv::Mat image(
            static_cast<int>(frameHeader.rows),
            static_cast<int>(frameHeader.cols),
            static_cast<int>(frameHeader.type),
            rawBuffer.data()
        );

        // clone 一份，避免 rawBuffer 生命周期结束后 image 数据失效
        cv::Mat imageClone = image.clone();

        if (imageClone.type() == CV_8UC3) {
            std::string ppmFilename =
                "frame_" + std::to_string(frameHeader.frameId) + ".ppm";

            saveAsPpmForView(ppmFilename, imageClone);
        }
    }

    close(clientSocket);
    close(serverSocket);

    return 0;
}