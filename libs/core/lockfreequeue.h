#pragma once
#include <atomic>
#include <array>

template<typename T, size_t Size>
class LockFreeQueue {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    static constexpr size_t MASK = Size - 1;  // Bit mask for modulo

public:
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) & MASK;  // BIT MASK instead of %
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // Queue full
        }
        
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    bool pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue empty
        }
        
        item = buffer_[current_head];
        head_.store((current_head + 1) & MASK, std::memory_order_release);  // BIT MASK
        return true;
    }
    
    // Non-blocking size check (approximate due to concurrent access)
    size_t size() const {
        size_t current_head = head_.load(std::memory_order_relaxed);
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        return (current_tail - current_head) & MASK;
    }
    
    bool empty() const {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }
    
    bool full() const {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) & MASK;
        return next_tail == head_.load(std::memory_order_relaxed);
    }

private:
    std::array<T, Size> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

// Forward declarations
struct Trade;
struct OrderBook;

// QUEUE SIZES: 65k trades = ~3.3s at 20k msg/s (prevents GUI thread hiccup overflow)
using TradeQueue = LockFreeQueue<Trade, 65536>;      // 2^16
using OrderBookQueue = LockFreeQueue<OrderBook, 16384>;  // 2^14 