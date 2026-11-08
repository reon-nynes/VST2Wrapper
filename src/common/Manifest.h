#pragma once
#include "Types.h"

namespace nytera
{
struct ParameterInfo
{
    uint32_t index = 0;
    uint32_t vst3Id = 0;
    std::string name;
    std::string label;
    float initialValue = 0;
    bool automatable = true;
    bool operator==(const ParameterInfo&) const = default;
};

struct PinInfo
{
    std::string name;
    bool active = true;
    bool operator==(const PinInfo&) const = default;
};

struct PayloadEntry
{
    std::string path;
    Hash256 hash {};
    uint64_t size = 0;
    uint32_t resourceId = 0;
    bool operator==(const PayloadEntry&) const = default;
};

struct Manifest
{
    uint32_t schemaVersion = 1;
    Id128 conversionId {}, processorId {}, controllerId {};
    Hash256 sourceHash {};
    uint32_t sourceUniqueId = 0;
    Architecture architecture = Architecture::x64;
    PluginKind kind = PluginKind::effect;
    std::string dllName;
    std::string displayName;
    std::string sourceVendor;
    std::vector<ParameterInfo> parameters;
    std::vector<PinInfo> inputs, outputs;
    bool midiInput = false;
    bool midiOutput = false;
    bool hasEditor = true;
    Size editorSize;
    uint32_t initialDelay = 0;
    double tailSeconds = 0;
    std::vector<PayloadEntry> payloads;
    bool operator==(const Manifest&) const = default;
};

Result<Architecture> inspectPeArchitecture(const std::filesystem::path&);
Bytes encodeManifest(const Manifest&);
Result<Manifest> decodeManifest(std::span<const std::byte>);
Manifest assignBindingIds(Manifest);
}
