// Project Headers
#include "FrameBufferPool.h"

#include <iostream>

FrameBufferPool::FrameBufferPool(size_t bufferSize, size_t bufferCount)
    : bufferSize_(bufferSize),
      bufferCount_(bufferCount) {
    storage_.reserve(bufferCount_);

    for (size_t i = 0; i < bufferCount_; ++i) {
        std::unique_ptr<uint8_t[]> buffer(new uint8_t[bufferSize_]);
        freeBuffers_.push(buffer.get());
        storage_.push_back(std::move(buffer));
    }

    std::cout << "[FrameBufferPool] 初始化完成: "
              << "bufferSize=" << bufferSize_
              << ", bufferCount=" << bufferCount_
              << ", totalMemory=" << (bufferSize_ * bufferCount_ / 1024 / 1024)
              << "MB" << std::endl;
}

FrameBufferPool::BufferPtr FrameBufferPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);

    condition_.wait(lock, [this]() {
        return !freeBuffers_.empty();
    });

    uint8_t* raw = freeBuffers_.front();
    freeBuffers_.pop();

    return BufferPtr(raw, [this](uint8_t* ptr) {
        this->release(ptr);
    });
}

void FrameBufferPool::release(uint8_t* buffer) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        freeBuffers_.push(buffer);
    }
    condition_.notify_one();
}

size_t FrameBufferPool::bufferSize() const {
    return bufferSize_;
}

size_t FrameBufferPool::totalCount() const {
    return bufferCount_;
}

size_t FrameBufferPool::availableCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return freeBuffers_.size();
}