#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t maxSize = 10)
        : maxSize_(maxSize),
          stopped_(false) {}

    void push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (stopped_) {
            return;
        }

        if (queue_.size() >= maxSize_) {
            queue_.pop();
        }

        queue_.push(item);
        condition_.notify_one();
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        condition_.wait(lock, [this]() {
            return stopped_ || !queue_.empty();
        });

        if (stopped_ && queue_.empty()) {
            return false;
        }

        item = queue_.front();
        queue_.pop();

        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }

        condition_.notify_all();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<T> queue_;
    size_t maxSize_;
    bool stopped_;
};

#endif