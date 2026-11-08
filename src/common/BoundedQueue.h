#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace nytera
{
// Sequence numbers reserve each cell before publishing its payload. The queue
// supports callbacks from the source's GUI and processing threads concurrently.
template<class T, size_t Capacity> class BoundedQueue
{
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
    struct Cell { std::atomic<size_t> sequence; T value; };
public:
    BoundedQueue()
    {
        for (size_t i = 0; i < Capacity; ++i) cells[i].sequence.store(i, std::memory_order_relaxed);
    }
    bool push(const T& value) noexcept
    {
        auto position = write.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;)
        {
            cell = &cells[position & (Capacity - 1)];
            auto difference = static_cast<intptr_t>(cell->sequence.load(std::memory_order_acquire) - position);
            if (difference == 0)
            {
                if (write.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) break;
            }
            else if (difference < 0) return false;
            else position = write.load(std::memory_order_relaxed);
        }
        cell->value = value;
        cell->sequence.store(position + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept
    {
        auto position = read.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;)
        {
            cell = &cells[position & (Capacity - 1)];
            auto difference = static_cast<intptr_t>(cell->sequence.load(std::memory_order_acquire) - (position + 1));
            if (difference == 0)
            {
                if (read.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) break;
            }
            else if (difference < 0) return false;
            else position = read.load(std::memory_order_relaxed);
        }
        value = cell->value;
        cell->sequence.store(position + Capacity, std::memory_order_release);
        return true;
    }
private:
    std::array<Cell, Capacity> cells;
    alignas(64) std::atomic<size_t> write {0};
    alignas(64) std::atomic<size_t> read {0};
};
}
