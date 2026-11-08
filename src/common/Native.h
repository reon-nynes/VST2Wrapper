#pragma once
#include "Types.h"
#include <windows.h>
#include <utility>
namespace nytera
{
class Handle
{
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) { reset(); value = std::exchange(other.value, nullptr); } return *this; }
    void reset(HANDLE next = nullptr) { if (valid()) CloseHandle(value); value = next; }
    HANDLE get() const { return value; }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
private:
    HANDLE value = nullptr;
};
inline std::wstring quoteArgument(const std::wstring& argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t character : argument)
    {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
}
