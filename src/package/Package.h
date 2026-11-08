#pragma once
#include "common/Manifest.h"
#include <windows.h>
#include <functional>
namespace nytera
{
struct PackageInput
{
    Manifest manifest;
    std::filesystem::path wrapperTemplate, helper, source, supportDirectory;
};
Status writePackage(const PackageInput&, const std::filesystem::path& output, std::function<bool()> cancelled = {});
Result<Manifest> readModuleManifest(HMODULE);
Result<Manifest> readPackageManifest(const std::filesystem::path&);
Bytes readResource(HMODULE, uint32_t resourceId);
std::filesystem::path relativePayloadPath(const std::string&);
}
