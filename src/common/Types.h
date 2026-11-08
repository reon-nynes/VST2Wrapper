#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace nytera
{
using Bytes = std::vector<std::byte>;
using Id128 = std::array<std::byte, 16>;
using Hash256 = std::array<std::byte, 32>;

enum class ErrorCode : uint32_t
{
    invalidData, io, unsupported, architecture, plugin, timeout, disconnected, identity
};

struct Failure
{
    ErrorCode code = ErrorCode::invalidData;
    std::string message;
};

template<class T> using Result = std::variant<T, Failure>;
using Status = Result<std::monostate>;

enum class Architecture : uint32_t { x86 = 1, x64 = 2 };
enum class PluginKind : uint32_t { effect = 0, instrument = 1 };

struct Size
{
    uint32_t width = 480;
    uint32_t height = 320;
    bool operator==(const Size&) const = default;
};

std::string pathUtf8(const std::filesystem::path&);
std::filesystem::path utf8Path(const std::string&);
Bytes readFile(const std::filesystem::path&);
void writeFile(const std::filesystem::path&, std::span<const std::byte>);
Hash256 sha256(std::span<const std::byte>);
std::string hex(std::span<const std::byte>);
}
