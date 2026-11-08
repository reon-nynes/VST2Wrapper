#pragma once
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

struct TestCase { std::string name; std::function<void()> run; };
std::vector<TestCase>& testCases();
struct RegisterTest
{
    RegisterTest(const char* name, std::function<void()> run) { testCases().push_back({name, std::move(run)}); }
};
#define NY_JOIN_IMPL(a, b) a##b
#define NY_JOIN(a, b) NY_JOIN_IMPL(a, b)
#define TEST(name) static void NY_JOIN(test_, __LINE__)(); \
    static RegisterTest NY_JOIN(reg_, __LINE__)(name, NY_JOIN(test_, __LINE__)); \
    static void NY_JOIN(test_, __LINE__)()
#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #expression); } while (false)

template<class T> T require(nytera::Result<T> result)
{
    if (auto* failure = std::get_if<nytera::Failure>(&result)) throw std::runtime_error(failure->message);
    return std::get<T>(std::move(result));
}
