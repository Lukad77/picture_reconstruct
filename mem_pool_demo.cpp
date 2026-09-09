#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstring>
#include <chrono>

class MemoryPool {
public:
    struct Buffer {
        uint8_t* data = nullptr;
        size_t capacity = 0;
        size_t size = 0;   // 实际写入的数据长度
        uint64_t id = 0;   // 方便调试和跟踪
    };

    using BufferPtr = std::shared_ptr<Buffer>;

public:
    MemoryPool(size_t blockSize, size_t blockCount)
        : blockSize_(blockSize), blockCount_(blockCount), stop_(false), nextId_(0) {
        storage_.reserve(blockCount_);
        for (size_t i = 0; i < blockCount_; ++i) {
            auto raw = std::make_unique<uint8_t[]>(blockSize_);
            auto buf = std::make_unique<Buffer>();
            buf->data = raw.get();
            buf->capacity = blockSize_;
            buf->size = 0;
            buf->id = nextId_++;

            rawBlocks_.push_back(std::move(raw));
            buffers_.push_back(std::move(buf));
        }

        for (auto& buf : buffers_) {
            freeList_.push(buf.get());
        }
    }

    ~MemoryPool() {
        shutdown();
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // 阻塞获取一个可用 buffer
    BufferPtr acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]() {
            return stop_ || !freeList_.empty();
        });

        if (stop_) {
            return nullptr;
        }

        Buffer* buf = freeList_.front();
        freeList_.pop();
        buf->size = 0;

        ++inUseCount_;
        updateHighWaterMark();

        // 用 shared_ptr + 自定义 deleter 实现自动归还
        return BufferPtr(buf, [this](Buffer* p) {
            this->release(p);
        });
    }

    // 尝试非阻塞获取
    BufferPtr tryAcquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (freeList_.empty() || stop_) {
            ++acquireFailCount_;
            return nullptr;
        }

        Buffer* buf = freeList_.front();
        freeList_.pop();
        buf->size = 0;

        ++inUseCount_;
        updateHighWaterMark();

        return BufferPtr(buf, [this](Buffer* p) {
            this->release(p);
        });
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        cond_.notify_all();
    }

    size_t blockSize() const { return blockSize_; }
    size_t blockCount() const { return blockCount_; }

    size_t inUseCount() const { return inUseCount_.load(); }
    size_t highWaterMark() const { return highWaterMark_.load(); }
    size_t acquireFailCount() const { return acquireFailCount_.load(); }

private:
    void release(Buffer* buf) {
        if (!buf) return;

        std::lock_guard<std::mutex> lock(mutex_);
        buf->size = 0;
        freeList_.push(buf);

        if (inUseCount_ > 0) {
            --inUseCount_;
        }

        cond_.notify_one();
    }

    void updateHighWaterMark() {
        size_t cur = inUseCount_.load();
        size_t old = highWaterMark_.load();
        while (cur > old && !highWaterMark_.compare_exchange_weak(old, cur)) {
            // 自旋更新高水位
        }
    }

private:
    size_t blockSize_;
    size_t blockCount_;
    bool stop_;

    std::vector<std::unique_ptr<uint8_t[]>> rawBlocks_;
    std::vector<std::unique_ptr<Buffer>> buffers_;
    std::queue<Buffer*> freeList_;

    mutable std::mutex mutex_;
    std::condition_variable cond_;

    std::atomic<size_t> inUseCount_{0};
    std::atomic<size_t> highWaterMark_{0};
    std::atomic<size_t> acquireFailCount_{0};
    std::atomic<uint64_t> nextId_;
};
