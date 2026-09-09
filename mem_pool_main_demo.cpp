#include <queue>

class FrameQueue {
public:
    void push(MemoryPool::BufferPtr buf) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(buf));
        }
        cond_.notify_one();
    }

    bool pop(MemoryPool::BufferPtr& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]() {
            return stop_ || !queue_.empty();
        });

        if (stop_ && queue_.empty()) {
            return false;
        }

        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cond_.notify_all();
    }

private:
    std::queue<MemoryPool::BufferPtr> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool stop_ = false;
};

// 模拟采集线程
void producer(MemoryPool& pool, FrameQueue& q, std::atomic<bool>& running) {
    int frameNo = 0;

    while (running) {
        auto buf = pool.acquire();
        if (!buf) break;

        // 模拟一帧图像数据
        std::string fakeFrame = "frame_" + std::to_string(frameNo++);
        size_t len = fakeFrame.size();

        if (len > buf->capacity) {
            std::cerr << "frame too large for pool block\n";
            continue;
        }

        std::memcpy(buf->data, fakeFrame.data(), len);
        buf->size = len;

        q.push(std::move(buf));

        // 模拟相机高频采集
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// 模拟网络发送线程
void consumer(FrameQueue& q) {
    MemoryPool::BufferPtr buf;
    while (q.pop(buf)) {
        // 模拟发送
        std::string payload(reinterpret_cast<char*>(buf->data), buf->size);
        std::cout << "send: " << payload
                  << ", buffer_id=" << buf->id
                  << ", size=" << buf->size << "\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // buf 离开作用域后自动归还内存池
    }
}

int main() {
    constexpr size_t blockSize = 1024 * 1024; // 1MB
    constexpr size_t blockCount = 8;          // 8 个 buffer

    MemoryPool pool(blockSize, blockCount);
    FrameQueue queue;
    std::atomic<bool> running{true};

    std::thread p1(producer, std::ref(pool), std::ref(queue), std::ref(running));
    std::thread c1(consumer, std::ref(queue));

    std::this_thread::sleep_for(std::chrono::seconds(2));
    running = false;

    p1.join();
    queue.shutdown();
    c1.join();
    pool.shutdown();

    std::cout << "\n=== pool stats ===\n";
    std::cout << "in use: " << pool.inUseCount() << "\n";
    std::cout << "high water mark: " << pool.highWaterMark() << "\n";
    std::cout << "acquire fail count: " << pool.acquireFailCount() << "\n";

    return 0;
}
