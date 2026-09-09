#ifndef FRAME_BUFFER_POOL_H
#define FRAME_BUFFER_POOL_H

// C++ Standard Library
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <cstddef>

class FrameBufferPool {
public:
    using BufferPtr = std::shared_ptr<uint8_t[]>;

    FrameBufferPool(size_t bufferSize, size_t bufferCount);

    BufferPtr acquire();

    size_t bufferSize() const;
    size_t totalCount() const;
    size_t availableCount() const;

private:
    void release(uint8_t* buffer);

private:
    size_t bufferSize_;
    size_t bufferCount_;

    std::vector<std::unique_ptr<uint8_t[]>> storage_;
    std::queue<uint8_t*> freeBuffers_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

#endif