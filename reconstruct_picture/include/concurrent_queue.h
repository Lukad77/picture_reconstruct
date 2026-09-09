#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

template <typename T>
class BoundedQueue {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    size_t max_size_;
    std::atomic<bool> finished_{false};

public:
    explicit BoundedQueue(size_t max_size = 100) : max_size_(max_size) {}

    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this]() { return queue_.size() < max_size_; });
        queue_.push(std::move(item));
        lock.unlock();
        not_empty_.notify_one(); 
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this]() { return !queue_.empty() || finished_; });

        if (queue_.empty() && finished_) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one(); 
        return true;
    }

    void setFinished() {
        finished_ = true;
        not_empty_.notify_all(); 
    }
};