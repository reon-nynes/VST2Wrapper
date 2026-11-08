#include "common/Types.h"
#include "Test.h"
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
