#pragma once
#include "Package.h"
#include "common/Native.h"
namespace nytera
{
struct ExtractedPayload
{
    std::filesystem::path directory, source, helper;
    Handle lease;
};
Result<ExtractedPayload> ensurePayload(HMODULE, const Manifest&, const std::filesystem::path& cacheRoot);
std::filesystem::path defaultCacheRoot();
}
