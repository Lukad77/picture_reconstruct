#include "RawFrameSender.h"
#include "RawFrameProtocol.h"

#include <iostream>
#include <vector>
#include <algorithm>

RawFrameSender::RawFrameSender(TcpClient& client)
    : client_(client),
      fragmentSize_(64 * 1024) {}

void RawFrameSender::setFragmentSize(uint32_t fragmentSize) {
    if (fragmentSize < 1024) {
        fragmentSize = 1024;
    }

    fragmentSize_ = fragmentSize;
}

bool RawFrameSender::sendFrame(const RawFrame& frame) {
    if (frame.empty()) {
        std::cerr << "[RawFrameSender] 空帧，无法发送" << std::endl;
        return false;
    }

    uint64_t totalBytes = frame.totalBytes;

    if (totalBytes == 0) {
        std::cerr << "[RawFrameSender] 图像字节数为 0" << std::endl;
        return false;
    }

    uint32_t fragmentCount = static_cast<uint32_t>(
        (totalBytes + fragmentSize_ - 1) / fragmentSize_
    );

    RawFrameHeader frameHeader{};
    frameHeader.frameId = frame.frameId;
    frameHeader.rows = frame.rows;
    frameHeader.cols = frame.cols;
    frameHeader.type = frame.type;
    frameHeader.elemSize = frame.elemSize;
    frameHeader.totalBytes = totalBytes;
    frameHeader.fragmentSize = fragmentSize_;
    frameHeader.fragmentCount = fragmentCount;

    std::vector<uint8_t> frameHeaderBytes =
        RawFrameProtocol::buildFrameHeader(frameHeader);

    if (!client_.sendAll(frameHeaderBytes.data(), frameHeaderBytes.size())) {
        std::cerr << "[RawFrameSender] 发送帧头失败" << std::endl;
        return false;
    }

    const uint8_t* rawData = frame.data();

    for (uint32_t index = 0; index < fragmentCount; ++index) {
        uint64_t offset = static_cast<uint64_t>(index) * fragmentSize_;
        uint64_t remain = totalBytes - offset;

        uint32_t payloadSize = static_cast<uint32_t>(
            std::min<uint64_t>(fragmentSize_, remain)
        );

        RawChunkHeader chunkHeader{};
        chunkHeader.frameId = frame.frameId;
        chunkHeader.fragmentIndex = index;
        chunkHeader.payloadSize = payloadSize;
        chunkHeader.offset = offset;

        std::vector<uint8_t> chunkHeaderBytes =
            RawFrameProtocol::buildChunkHeader(chunkHeader);

        if (!client_.sendAll(chunkHeaderBytes.data(), chunkHeaderBytes.size())) {
            std::cerr << "[RawFrameSender] 发送分片头失败" << std::endl;
            return false;
        }

        if (!client_.sendAll(rawData + offset, payloadSize)) {
            std::cerr << "[RawFrameSender] 发送分片数据失败" << std::endl;
            return false;
        }
    }

    std::cout << "[RawFrameSender] 已发送原始帧 frameId="
              << frame.frameId
              << ", size=" << totalBytes
              << ", fragments=" << fragmentCount
              << std::endl;

    return true;
}