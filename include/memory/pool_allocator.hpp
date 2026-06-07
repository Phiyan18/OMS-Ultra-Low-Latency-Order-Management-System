#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace oms {

// Lock-free Treiber stack free-list. Zero heap allocation after init.
template <typename T, std::size_t Capacity>
class PoolAllocator {
public:
    static_assert(Capacity > 0, "Capacity must be positive");

    PoolAllocator() {
        for (std::size_t i = 0; i < Capacity; ++i) {
            nodes_[i].next.store(i + 1 < Capacity ? static_cast<int32_t>(i + 1) : -1,
                                 std::memory_order_relaxed);
        }
        free_head_.store(0, std::memory_order_relaxed);
        allocated_.store(0, std::memory_order_relaxed);
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    T* acquire() noexcept {
        int32_t head = free_head_.load(std::memory_order_acquire);
        while (head >= 0) {
            int32_t next = nodes_[static_cast<std::size_t>(head)].next.load(std::memory_order_relaxed);
            if (free_head_.compare_exchange_weak(head, next, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
                allocated_.fetch_add(1, std::memory_order_relaxed);
                return reinterpret_cast<T*>(&nodes_[static_cast<std::size_t>(head)].storage);
            }
        }
        return nullptr;
    }

    void release(T* ptr) noexcept {
        if (!ptr) return;
        auto* node = reinterpret_cast<Node*>(reinterpret_cast<char*>(ptr) -
                                             offsetof(Node, storage));
        std::size_t idx = static_cast<std::size_t>(node - nodes_.data());
        int32_t head = free_head_.load(std::memory_order_relaxed);
        do {
            node->next.store(head, std::memory_order_relaxed);
        } while (!free_head_.compare_exchange_weak(head, static_cast<int32_t>(idx),
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed));
        allocated_.fetch_sub(1, std::memory_order_relaxed);
    }

    std::size_t capacity() const noexcept { return Capacity; }
    std::size_t allocated() const noexcept { return allocated_.load(std::memory_order_relaxed); }
    std::size_t available() const noexcept { return Capacity - allocated(); }

private:
    struct alignas(T) Node {
        alignas(T) std::byte storage[sizeof(T)];
        std::atomic<int32_t> next{-1};
    };

    std::array<Node, Capacity> nodes_{};
    std::atomic<int32_t> free_head_{-1};
    std::atomic<std::size_t> allocated_{0};
};

}  // namespace oms
