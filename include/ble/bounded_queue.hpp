#pragma once

// Fixed-capacity, mutex-protected FIFO used to hand events from BLE/web tasks
// to the cooperative main loop. push() never blocks on a full queue: it drops
// the new item and counts it, so producers (callbacks) stay non-blocking.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace smartfitness {

template <typename T, std::size_t Capacity>
class BoundedQueue {
    static_assert(Capacity > 0, "queue capacity must be positive");

public:
    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == Capacity) {
            ++dropped_;
            return false;
        }
        items_[(head_ + count_) % Capacity] = item;
        ++count_;
        return true;
    }

    // Critical events can displace the oldest expendable item, without
    // reordering the remaining FIFO (notably reads before Connected).
    template <typename Predicate>
    bool pushOrEvict(const T& item, Predicate expendable, bool& evicted) {
        std::lock_guard<std::mutex> lock(mutex_);
        evicted = false;
        if (count_ == Capacity) {
            std::size_t index = 0;
            while (index < count_ && !expendable(items_[(head_ + index) % Capacity])) ++index;
            ++dropped_;
            if (index == count_) return false;
            for (; index + 1 < count_; ++index)
                items_[(head_ + index) % Capacity] = items_[(head_ + index + 1) % Capacity];
            --count_;
            evicted = true;
        }
        items_[(head_ + count_) % Capacity] = item;
        ++count_;
        return true;
    }

    bool pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) return false;
        item = items_[head_];
        head_ = (head_ + 1) % Capacity;
        --count_;
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        count_ = 0;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    std::uint32_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    mutable std::mutex mutex_;
    std::array<T, Capacity> items_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::uint32_t dropped_ = 0;
};

}  // namespace smartfitness
