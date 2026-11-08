#include "Types.h"
#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <stdexcept>

namespace nytera
{
std::string pathUtf8(const std::filesystem::path& path)
{
    auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::filesystem::path utf8Path(const std::string& text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
Bytes readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open " + pathUtf8(path));
    auto length = file.tellg();
    if (length < 0 || uint64_t(length) > 1024ull * 1024 * 1024)
        throw std::runtime_error("File exceeds 1 GiB payload limit");
    Bytes bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read " + pathUtf8(path));
    return bytes;
}
void writeFile(const std::filesystem::path& path, std::span<const std::byte> data)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Cannot write " + pathUtf8(path));
    file.close();
    if (!file) throw std::runtime_error("Cannot finish " + pathUtf8(path));
}
Hash256 sha256(std::span<const std::byte> bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto check = [](NTSTATUS status) { if (status < 0) throw std::runtime_error("SHA-256 operation failed"); };
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    try
    {
        check(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
        while (!bytes.empty())
        {
            auto length = static_cast<ULONG>(std::min<size_t>(bytes.size(), 1024 * 1024));
            check(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), length, 0));
            bytes = bytes.subspan(length);
        }
        Hash256 result;
        check(BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(result.data()), static_cast<ULONG>(result.size()), 0));
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    }
    catch (...)
    {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}
std::string hex(std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto value : bytes)
    {
        auto byte = std::to_integer<uint8_t>(value);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
}
