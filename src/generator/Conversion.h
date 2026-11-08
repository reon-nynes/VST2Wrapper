#pragma once
#include "common/Manifest.h"
#include <functional>
#include <optional>
namespace nytera
{
struct RuntimeAssets
{
    std::filesystem::path x86Helper, x64Helper, effectTemplate, instrumentTemplate;
    static RuntimeAssets fromDirectory(const std::filesystem::path&);
    const std::filesystem::path& helper(Architecture architecture) const { return architecture == Architecture::x86 ? x86Helper : x64Helper; }
};
struct InspectionResult
{
    Manifest manifest;
    std::filesystem::path helper;
    std::vector<std::string> imports;
    std::string report;
};
struct ConversionRequest
{
    std::filesystem::path source, supportDirectory, output;
    std::optional<InspectionResult> inspected;
};
using ProgressCallback = std::function<bool(double, const std::string&)>;
using CancelCallback = std::function<bool()>;
Result<InspectionResult> inspectSource(const std::filesystem::path&, const RuntimeAssets&,
    const std::filesystem::path& supportDirectory = {}, CancelCallback cancelled = {});
Status generateConversion(const ConversionRequest&, const RuntimeAssets&, ProgressCallback = {});
std::string outputFilename(const Manifest&);
}
