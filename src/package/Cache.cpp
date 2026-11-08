#include "Cache.h"
#include <algorithm>
#include <chrono>
#include <set>
#include <bcrypt.h>
namespace nytera
{
static void rejectReparse(const std::filesystem::path& path)
{
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    auto current = absolute.root_path();
    for (const auto& component : absolute.relative_path())
    {
        current /= component;
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Cache destination contains a reparse point");
    }
}
struct CacheLock
{
    Handle mutex;
    bool owned = false;
    explicit CacheLock(const std::wstring& name, DWORD timeout = 60000) : mutex(CreateMutexW(nullptr, FALSE, name.c_str()))
    {
        if (!mutex.valid()) throw std::runtime_error("Cannot create cache coordination mutex");
        auto result = WaitForSingleObject(mutex.get(), timeout);
        owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        if (!owned) throw std::runtime_error("Cache coordination timed out");
    }
    ~CacheLock() { if (owned) ReleaseMutex(mutex.get()); }
};
static bool validCache(const std::filesystem::path& directory, const Manifest& manifest)
{
    try
    {
        rejectReparse(directory);
        if (readFile(directory / L".nytera.complete") != encodeManifest(manifest)) return false;
        for (const auto& entry : manifest.payloads)
        {
            auto destination = directory / relativePayloadPath(entry.path);
            rejectReparse(destination);
            if (!std::filesystem::is_regular_file(destination) || std::filesystem::file_size(destination) != entry.size
                || sha256(readFile(destination)) != entry.hash) return false;
        }
        return true;
    }
    catch (...) { return false; }
}
static void cleanupCache(const std::filesystem::path& root, const std::filesystem::path& active)
{
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 30);
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
        try
        {
            if (entry.path() == active || !entry.is_directory()) continue;
            rejectReparse(entry.path());
            auto marker = entry.path() / L".nytera.complete";
            if (!std::filesystem::exists(marker) || std::filesystem::last_write_time(marker) >= cutoff) continue;
            auto mutexName = L"Local\\NyteraCache-" + entry.path().filename().wstring();
            CacheLock lock(mutexName, 0);
            Handle exclusive(CreateFileW((entry.path() / L".nytera.lease").c_str(), GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!exclusive.valid()) continue;
            exclusive.reset();
            std::filesystem::remove_all(entry.path());
        }
        catch (...) {}
    }
}
Result<ExtractedPayload> ensurePayload(HMODULE module, const Manifest& manifest, const std::filesystem::path& cacheRoot)
{
    std::filesystem::path staging;
    try
    {
        if (cacheRoot.empty()) throw std::runtime_error("Cache root is unavailable");
        auto root = std::filesystem::absolute(cacheRoot).lexically_normal();
        rejectReparse(root);
        std::filesystem::create_directories(root);
        rejectReparse(root);
        auto key = hex(sha256(encodeManifest(manifest))) + "-v1";
        auto directory = root / utf8Path(key);
        std::set<std::wstring> destinations;
        std::set<uint32_t> ids;
        const PayloadEntry* dll = nullptr;
        const PayloadEntry* helper = nullptr;
        for (const auto& entry : manifest.payloads)
        {
            auto path = relativePayloadPath(entry.path);
            auto normalized = path.wstring();
            CharUpperBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
            if (!destinations.insert(normalized).second || !ids.insert(entry.resourceId).second
                || entry.resourceId < 102 || entry.resourceId > 65000 || entry.size > 1024ull * 1024 * 1024)
                throw std::runtime_error("Invalid payload table");
            if (entry.resourceId == 103) dll = &entry;
            if (entry.resourceId == 102) helper = &entry;
        }
        if (!dll || !helper || dll->hash != manifest.sourceHash || dll->path != manifest.dllName || helper->path != "NyteraBridge.exe")
            throw std::runtime_error("Package is missing its bound DLL or helper");
        CacheLock lock(L"Local\\NyteraCache-" + utf8Path(key).wstring());
        for (const auto& entry : manifest.payloads) rejectReparse(directory / relativePayloadPath(entry.path));
        if (!validCache(directory, manifest))
        {
            rejectReparse(directory);
            if (std::filesystem::exists(directory))
            {
                Handle exclusive(CreateFileW((directory / L".nytera.lease").c_str(), GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (!exclusive.valid()) throw std::runtime_error("Corrupted cache is still in use by another instance");
                exclusive.reset();
                std::filesystem::remove_all(directory);
            }
            Id128 random;
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(random.data()), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                throw std::runtime_error("Cannot create extraction identity");
            staging = root / utf8Path(key + ".tmp-" + hex(random));
            std::filesystem::create_directory(staging);
            for (const auto& entry : manifest.payloads)
            {
                auto destination = staging / relativePayloadPath(entry.path);
                rejectReparse(destination);
                std::filesystem::create_directories(destination.parent_path());
                auto bytes = readResource(module, entry.resourceId);
                if (bytes.size() != entry.size || sha256(bytes) != entry.hash) throw std::runtime_error("Embedded payload hash mismatch");
                writeFile(destination, bytes);
            }
            writeFile(staging / L".nytera.complete", encodeManifest(manifest));
            writeFile(staging / L".nytera.lease", {});
            if (!MoveFileExW(staging.c_str(), directory.c_str(), MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot publish extracted payload");
            staging.clear();
        }
        ExtractedPayload result;
        result.directory = directory;
        result.source = directory / relativePayloadPath(dll->path);
        result.helper = directory / relativePayloadPath(helper->path);
        result.lease.reset(CreateFileW((directory / L".nytera.lease").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!result.lease.valid()) throw std::runtime_error("Cannot lease extracted payload");
        std::filesystem::last_write_time(directory / L".nytera.complete", std::filesystem::file_time_type::clock::now());
        cleanupCache(root, directory);
        return result;
    }
    catch (const std::exception& error)
    {
        if (!staging.empty())
        {
            std::error_code ignored;
            std::filesystem::remove_all(staging, ignored);
        }
        return Failure{ErrorCode::io, error.what()};
    }
}
std::filesystem::path defaultCacheRoot()
{
    std::wstring path(32768, L'\0');
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path) / L"Nytera/VST2Wrapper/Cache";
}
}
