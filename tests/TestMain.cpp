#include "common/Types.h"
#include "Test.h"
#include <cstdlib>
#include <new>
thread_local bool countAllocations = false;
thread_local size_t allocationCount = 0;
void* operator new(size_t size)
{
    if (countAllocations) ++allocationCount;
    if (auto* data = std::malloc(size == 0 ? 1 : size)) return data;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* data) noexcept { std::free(data); }
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete(void* data, size_t) noexcept { std::free(data); }
void operator delete[](void* data, size_t) noexcept { std::free(data); }
std::vector<TestCase>& testCases() { static std::vector<TestCase> cases; return cases; }
int main(int argc, char** argv)
{
    int failures = 0;
    int executed = 0;
    for (const auto& test : testCases())
    {
        if (argc > 1 && test.name != argv[1]) continue;
        ++executed;
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; }
    }
    if (executed == 0) { std::cerr << "No matching test\n"; return 2; }
    return failures == 0 ? 0 : 1;
}
