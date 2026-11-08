#include "common/Manifest.h"
#include "Test.h"
#include <limits>
using namespace nytera;

static Manifest example()
{
    Manifest manifest;
    manifest.dllName = "Old Plugin.dll";
    manifest.displayName = "Old Plugin";
    manifest.sourceVendor = "Example";
    manifest.sourceUniqueId = 0x12345678;
    manifest.sourceHash[0] = std::byte{42};
    manifest.parameters = {{0, 0, "Gain", "dB", 0.5f, true}, {1, 0, "Mode", "", 1.0f, false}};
    manifest.inputs = {{"Left", true}, {"Right", true}};
    manifest.outputs = manifest.inputs;
    manifest.midiInput = true;
    manifest.initialDelay = 17;
    manifest.tailSeconds = 2.5;
    manifest.payloads = {{"Old Plugin.dll", manifest.sourceHash, 1234, 103}};
    return manifest;
}

TEST("Manifest.RoundTrip")
{
    auto manifest = assignBindingIds(example());
    auto bytes = encodeManifest(manifest);
    CHECK(bytes.size() > 100);
    CHECK(require(decodeManifest(bytes)) == manifest);
}
TEST("Manifest.InfiniteTail")
{
    auto manifest = assignBindingIds(example());
    manifest.tailSeconds = std::numeric_limits<double>::infinity();
    CHECK(require(decodeManifest(encodeManifest(manifest))).tailSeconds == manifest.tailSeconds);
}
TEST("Manifest.ProgramMetadata")
{
    auto manifest = assignBindingIds(example());
    manifest.programs = {1, {"First", "Second"}};
    CHECK(require(decodeManifest(encodeManifest(manifest))) == manifest);
}
TEST("Manifest.LegacyVersion")
{
    auto manifest = assignBindingIds(example()); manifest.schemaVersion = 1;
    CHECK(require(decodeManifest(encodeManifest(manifest))) == manifest);
}

TEST("Manifest.Truncated")
{
    auto bytes = encodeManifest(example());
    CHECK(bytes.size() > 100);
    for (size_t length = 0; length < bytes.size(); ++length)
        CHECK(std::holds_alternative<Failure>(decodeManifest(std::span(bytes).first(length))));
    bytes[4] = std::byte{255};
    CHECK(std::holds_alternative<Failure>(decodeManifest(bytes)));
}

TEST("Manifest.Identity")
{
    auto first = assignBindingIds(example());
    CHECK(first.processorId != Id128{});
    CHECK(first.processorId != first.controllerId);
    auto renamed = example();
    renamed.dllName = "Renamed.dll";
    CHECK(assignBindingIds(renamed).processorId == first.processorId);
    renamed.sourceHash[0] = std::byte{43};
    CHECK(assignBindingIds(renamed).processorId != first.processorId);
    CHECK(first.parameters[0].vst3Id != first.parameters[1].vst3Id);
    CHECK(first.parameters[0].vst3Id >= 0x10000000 && first.parameters[0].vst3Id <= 0x1fffffff);
}

TEST("Manifest.PeArchitecture")
{
    auto root = std::filesystem::path(NYTERA_SOURCE_ROOT) / "vst2demo";
    CHECK(require(inspectPeArchitecture(root / "Decapitator.dll")) == Architecture::x86);
    for (auto filename : {"LexHall.dll", "LittleAlterBoy.dll", "VPRE-73.dll"})
        CHECK(require(inspectPeArchitecture(root / filename)) == Architecture::x64);
    CHECK(std::holds_alternative<Failure>(inspectPeArchitecture(root / "missing.dll")));
}
