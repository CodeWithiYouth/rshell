#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace rshell {

// One notification per connection, with bounded storage and reader backpressure.
// The UI consumes small batches; close() releases a reader waiting for space.
class ShellOutputBuffer {
public:
    explicit ShellOutputBuffer(std::size_t capacity = 8u * 1024u * 1024u)
        : capacity_(capacity ? capacity : 1) {}

    bool push(std::string data, const std::function<bool()>& notify) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            std::unique_lock<std::mutex> lock(mutex_);
            space_.wait(lock, [this]() { return closed_ || bytes_ < capacity_; });
            if (closed_) return false;
            const std::size_t count = std::min(data.size() - offset, capacity_ - bytes_);
            if (offset == 0 && count == data.size()) chunks_.push_back(std::move(data));
            else chunks_.push_back(data.substr(offset, count));
            bytes_ += count;
            offset += count;
            const bool post = !notified_;
            if (post) notified_ = true;
            lock.unlock();
            if (post && !notify()) {
                // Keep the bytes. The UI timer also drains pending buffers if the
                // Windows message queue was temporarily full.
                std::lock_guard<std::mutex> retry(mutex_);
                notified_ = false;
            }
        }
        return true;
    }

    std::string take(std::size_t limit) {
        std::unique_lock<std::mutex> lock(mutex_);
        std::string result;
        result.reserve(std::min(bytes_, limit));
        while (!chunks_.empty() && result.size() < limit) {
            std::string& front = chunks_.front();
            const std::size_t count = std::min(front.size(), limit - result.size());
            result.append(front, 0, count);
            bytes_ -= count;
            if (count == front.size()) chunks_.pop_front();
            else front.erase(0, count);
        }
        lock.unlock();
        space_.notify_all();
        return result;
    }

    bool finishBatch() {
        std::lock_guard<std::mutex> lock(mutex_);
        notified_ = !closed_ && bytes_ != 0;
        return notified_;
    }

    void notificationFailed() {
        std::lock_guard<std::mutex> lock(mutex_);
        notified_ = false;
    }

    std::size_t pendingBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            chunks_.clear();
            bytes_ = 0;
        }
        space_.notify_all();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable space_;
    std::deque<std::string> chunks_;
    std::size_t bytes_ = 0;
    bool notified_ = false;
    bool closed_ = false;
};

}  // namespace rshell
