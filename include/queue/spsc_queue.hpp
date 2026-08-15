#pragma once

#include "common/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace oms {

// Lock-free single-producer / single-consumer ring buffer.
// Producer may only call try_push / push; consumer may only call try_pop / pop.
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2 >= 2");

public:
    SPSCQueue() {
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    bool try_push(const T& item) noexcept {
        return emplace(item);
    }

    bool try_push(T&& item) noexcept {
        return emplace(std::move(item));
    }

    bool try_pop(T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        Slot& slot = slots_[head & kMask];
        const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                                   static_cast<std::intptr_t>(head + 1);
        if (diff != 0) return false;

        T* ptr = reinterpret_cast<T*>(&slot.storage);
        item = std::move(*ptr);
        ptr->~T();
        slot.sequence.store(head + Capacity, std::memory_order_release);
        head_.store(head + 1, std::memory_order_relaxed);
        return true;
    }

    bool empty() const noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        return head == tail;
    }

    std::size_t size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return tail - head;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    struct Slot {
        std::atomic<std::size_t> sequence{0};
        typename std::aligned_storage<sizeof(T), alignof(T)>::type storage{};
    };

    template <typename U>
    bool emplace(U&& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        Slot& slot = slots_[tail & kMask];
        const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                                   static_cast<std::intptr_t>(tail);
        if (diff != 0) return false;

        new (&slot.storage) T(std::forward<U>(item));
        slot.sequence.store(tail + 1, std::memory_order_release);
        tail_.store(tail + 1, std::memory_order_relaxed);
        return true;
    }

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    Slot slots_[Capacity]{};
};

}  // namespace oms
