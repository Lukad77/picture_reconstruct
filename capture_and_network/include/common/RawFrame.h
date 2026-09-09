#ifndef RAW_FRAME_H
#define RAW_FRAME_H

// C++ Standard Library
#include <cstdint>
#include <cstddef>
#include <memory>
#include <chrono>

struct RawFrame {
    uint64_t frameId;

    uint32_t rows;
    uint32_t cols;
    uint32_t type;
    uint32_t elemSize;

    uint64_t totalBytes;

    std::shared_ptr<uint16_t[]> buffer;

    std::chrono::steady_clock::time_point timestamp;

    RawFrame()
        : frameId(0),
          rows(0),
          cols(0),
          type(0),
          elemSize(0),
          totalBytes(0),
          timestamp(std::chrono::steady_clock::now()) {}

    RawFrame(
        uint64_t id,
        uint32_t r,
        uint32_t c,
        uint32_t t,
        uint32_t e,
        uint64_t bytes,
        std::shared_ptr<uint8_t[]> data
    )
        : frameId(id),
          rows(r),
          cols(c),
          type(t),
          elemSize(e),
          totalBytes(bytes),
          buffer(std::move(data)),
          timestamp(std::chrono::steady_clock::now()) {}

    bool empty() const {
        return !buffer || totalBytes == 0;
    }

    const uint8_t* data() const {
        return buffer.get();
    }
};

#endif