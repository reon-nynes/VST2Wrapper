#pragma once
#include "Types.h"
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nytera
{
class Writer
{
public:
    Bytes bytes;
    template<class T> void number(T value)
    {
        static_assert(std::is_integral_v<T>);
        using U = std::make_unsigned_t<T>;
        auto bits = static_cast<U>(value);
        for (size_t i = 0; i < sizeof(T); ++i)
            bytes.push_back(static_cast<std::byte>((bits >> (8 * i)) & 255));
    }
    void raw(std::span<const std::byte> data) { bytes.insert(bytes.end(), data.begin(), data.end()); }
    void text(const std::string& value)
    {
        if (value.size() > 65536) throw std::runtime_error("String exceeds protocol limit");
        number(static_cast<uint32_t>(value.size()));
        raw(std::as_bytes(std::span(value)));
    }
    void boolean(bool value) { number<uint8_t>(value ? 1 : 0); }
    void floating(float value) { number(std::bit_cast<uint32_t>(value)); }
    void floating(double value) { number(std::bit_cast<uint64_t>(value)); }
};

class Reader
{
public:
    explicit Reader(std::span<const std::byte> data) : remaining(data) {}
    std::span<const std::byte> raw(size_t count)
    {
        if (count > remaining.size()) throw std::runtime_error("Truncated data");
        auto result = remaining.first(count);
        remaining = remaining.subspan(count);
        return result;
    }
    template<class T> T number()
    {
        static_assert(std::is_integral_v<T>);
        auto data = raw(sizeof(T));
        uint64_t bits = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            bits |= uint64_t(std::to_integer<uint8_t>(data[i])) << (8 * i);
        return static_cast<T>(bits);
    }
    uint32_t count(uint32_t maximum)
    {
        auto value = number<uint32_t>();
        if (value > maximum) throw std::runtime_error("Count exceeds protocol limit");
        return value;
    }
    bool boolean()
    {
        auto value = number<uint8_t>();
        if (value > 1) throw std::runtime_error("Invalid boolean");
        return value == 1;
    }
    std::string text()
    {
        auto data = raw(count(65536));
        std::string result(reinterpret_cast<const char*>(data.data()), data.size());
        if (result.find('\0') != std::string::npos) throw std::runtime_error("Embedded NUL in string");
        return result;
    }
    template<size_t N> std::array<std::byte, N> array()
    {
        std::array<std::byte, N> result;
        auto data = raw(N);
        std::copy(data.begin(), data.end(), result.begin());
        return result;
    }
    float real32() { return std::bit_cast<float>(number<uint32_t>()); }
    double real64() { return std::bit_cast<double>(number<uint64_t>()); }
    void finish() const
    {
        if (!remaining.empty()) throw std::runtime_error("Unexpected trailing data");
    }
private:
    std::span<const std::byte> remaining;
};
}
