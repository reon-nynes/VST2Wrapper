#pragma once
#include <algorithm>
#include <chrono>
namespace nytera
{
class Deadline
{
public:
    explicit Deadline(std::chrono::milliseconds timeout) : end(std::chrono::steady_clock::now() + timeout) {}
    std::chrono::milliseconds remaining() const
    {
        return std::max(std::chrono::milliseconds::zero(), std::chrono::ceil<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()));
    }
private:
    std::chrono::steady_clock::time_point end;
};
}
