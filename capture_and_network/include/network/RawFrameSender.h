#ifndef RAW_FRAME_SENDER_H
#define RAW_FRAME_SENDER_H

#include <cstdint>

#include "RawFrame.h"
#include "TcpClient.h"

class RawFrameSender {
public:
    explicit RawFrameSender(TcpClient& client);

    void setFragmentSize(uint32_t fragmentSize);

    bool sendFrame(const RawFrame& frame);

private:
    TcpClient& client_;
    uint32_t fragmentSize_;
};

#endif