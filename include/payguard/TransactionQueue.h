#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace payguard {

// Bounded, blocking producer-consumer queue (mutex + two condition variables).
// push() blocks while full, which applies back-pressure to a fast reader;
// pop() blocks while empty and returns nullopt once the queue is closed and
// drained. Both are O(1).
template <typename T>
class TransactionQueue {
public:
    explicit TransactionQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("queue capacity must be positive");
        }
    }

    TransactionQueue(const TransactionQueue&) = delete;
    TransactionQueue& operator=(const TransactionQueue&) = delete;

    // Returns false (dropping the item) if the queue has been closed.
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
        if (closed_) {
            return false;
        }
        items_.push_back(std::move(item));
        lock.unlock();
        notEmpty_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) {
            return std::nullopt;  // closed and drained
        }
        T item = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        notFull_.notify_one();
        return item;
    }

    // No more pushes; consumers drain what is left, then see nullopt.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    std::size_t capacity() const { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace payguard
