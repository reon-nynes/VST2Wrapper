#include "Manifest.h"
#include "Codec.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace nytera
{
Result<Architecture> inspectPeArchitecture(const std::filesystem::path& path)
{
    try
    {
        auto data = readFile(path);
        Reader reader(data);
        if (reader.number<uint16_t>() != 0x5a4d) throw std::runtime_error("Missing DOS signature");
        reader.raw(0x3c - 2);
        auto offset = reader.number<uint32_t>();
        if (offset > data.size()) throw std::runtime_error("Invalid PE offset");
        Reader pe(std::span(data).subspan(offset));
        if (pe.number<uint32_t>() != 0x4550) throw std::runtime_error("Missing PE signature");
        switch (pe.number<uint16_t>())
        {
            case 0x014c: return Architecture::x86;
            case 0x8664: return Architecture::x64;
            default: return Failure{ErrorCode::architecture, "Only x86 and x64 DLLs are supported"};
        }
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}

Bytes encodeManifest(const Manifest& manifest)
{
    Writer writer;
    writer.number<uint32_t>(0x314d594e);
    writer.number(manifest.schemaVersion);
    writer.raw(manifest.conversionId);
    writer.raw(manifest.processorId);
    writer.raw(manifest.controllerId);
    writer.raw(manifest.sourceHash);
    writer.number(manifest.sourceUniqueId);
    writer.number(static_cast<uint32_t>(manifest.architecture));
    writer.number(static_cast<uint32_t>(manifest.kind));
    writer.text(manifest.dllName);
    writer.text(manifest.displayName);
    writer.text(manifest.sourceVendor);
    writer.number(static_cast<uint32_t>(manifest.parameters.size()));
    for (const auto& parameter : manifest.parameters)
    {
        writer.number(parameter.index);
        writer.number(parameter.vst3Id);
        writer.text(parameter.name);
        writer.text(parameter.label);
        writer.floating(parameter.initialValue);
        writer.boolean(parameter.automatable);
    }
    for (const auto* pins : {&manifest.inputs, &manifest.outputs})
    {
        writer.number(static_cast<uint32_t>(pins->size()));
        for (const auto& pin : *pins) { writer.text(pin.name); writer.boolean(pin.active); }
    }
    writer.boolean(manifest.midiInput);
    writer.boolean(manifest.midiOutput);
    writer.boolean(manifest.hasEditor);
    writer.number(manifest.editorSize.width);
    writer.number(manifest.editorSize.height);
    writer.number(manifest.initialDelay);
    writer.floating(manifest.tailSeconds);
    writer.number(static_cast<uint32_t>(manifest.payloads.size()));
    for (const auto& entry : manifest.payloads)
    {
        writer.text(entry.path);
        writer.raw(entry.hash);
        writer.number(entry.size);
        writer.number(entry.resourceId);
    }
    return std::move(writer.bytes);
}

Result<Manifest> decodeManifest(std::span<const std::byte> data)
{
    try
    {
        Reader reader(data);
        if (reader.number<uint32_t>() != 0x314d594e) throw std::runtime_error("Invalid manifest signature");
        Manifest manifest;
        manifest.schemaVersion = reader.number<uint32_t>();
        if (manifest.schemaVersion != 1) throw std::runtime_error("Unsupported manifest version");
        manifest.conversionId = reader.array<16>();
        manifest.processorId = reader.array<16>();
        manifest.controllerId = reader.array<16>();
        manifest.sourceHash = reader.array<32>();
        manifest.sourceUniqueId = reader.number<uint32_t>();
        manifest.architecture = static_cast<Architecture>(reader.number<uint32_t>());
        manifest.kind = static_cast<PluginKind>(reader.number<uint32_t>());
        if (manifest.architecture != Architecture::x86 && manifest.architecture != Architecture::x64)
            throw std::runtime_error("Invalid architecture");
        if (manifest.kind != PluginKind::effect && manifest.kind != PluginKind::instrument)
            throw std::runtime_error("Invalid plugin kind");
        manifest.dllName = reader.text();
        manifest.displayName = reader.text();
        manifest.sourceVendor = reader.text();
        auto parameterCount = reader.count(65536);
        for (uint32_t i = 0; i < parameterCount; ++i)
        {
            ParameterInfo parameter;
            parameter.index = reader.number<uint32_t>();
            parameter.vst3Id = reader.number<uint32_t>();
            parameter.name = reader.text();
            parameter.label = reader.text();
            parameter.initialValue = reader.real32();
            parameter.automatable = reader.boolean();
            if (parameter.index != i || !std::isfinite(parameter.initialValue)
                || parameter.initialValue < 0 || parameter.initialValue > 1)
                throw std::runtime_error("Invalid parameter descriptor");
            manifest.parameters.push_back(std::move(parameter));
        }
        for (auto* pins : {&manifest.inputs, &manifest.outputs})
        {
            auto count = reader.count(256);
            for (uint32_t i = 0; i < count; ++i) pins->push_back({reader.text(), reader.boolean()});
        }
        manifest.midiInput = reader.boolean();
        manifest.midiOutput = reader.boolean();
        manifest.hasEditor = reader.boolean();
        manifest.editorSize.width = reader.number<uint32_t>();
        manifest.editorSize.height = reader.number<uint32_t>();
        manifest.initialDelay = reader.number<uint32_t>();
        manifest.tailSeconds = reader.real64();
        if (manifest.editorSize.width > 32768 || manifest.editorSize.height > 32768
            || std::isnan(manifest.tailSeconds) || manifest.tailSeconds < 0)
            throw std::runtime_error("Invalid editor or tail dimensions");
        auto payloadCount = reader.count(64000);
        for (uint32_t i = 0; i < payloadCount; ++i)
        {
            PayloadEntry entry;
            entry.path = reader.text();
            entry.hash = reader.array<32>();
            entry.size = reader.number<uint64_t>();
            entry.resourceId = reader.number<uint32_t>();
            manifest.payloads.push_back(std::move(entry));
        }
        reader.finish();
        return manifest;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}

Manifest assignBindingIds(Manifest manifest)
{
    Writer seed;
    seed.raw(manifest.sourceHash);
    seed.number(manifest.sourceUniqueId);
    seed.number(static_cast<uint32_t>(manifest.architecture));
    auto id = [&](const std::string& domain)
    {
        Writer input;
        input.text("Nytera.VST2Wrapper.v1." + domain);
        input.raw(seed.bytes);
        auto hash = sha256(input.bytes);
        Id128 result;
        std::copy_n(hash.begin(), result.size(), result.begin());
        return result;
    };
    manifest.conversionId = id("binding");
    manifest.processorId = id("processor");
    manifest.controllerId = id("controller");
    std::set<uint32_t> used;
    for (auto& parameter : manifest.parameters)
    {
        Writer input;
        input.raw(manifest.conversionId);
        input.number(parameter.index);
        auto digest = sha256(input.bytes);
        Reader reader(digest);
        uint32_t number = 0x10000000 | (reader.number<uint32_t>() & 0x0fffffff);
        while (!used.insert(number).second) number = 0x10000000 | ((number + 1) & 0x0fffffff);
        parameter.vst3Id = number;
    }
    return manifest;
}
}
