#include "common/Codec.h"
#include "bridge/BridgeClient.h"
#include "Test.h"
#include <windows.h>
using namespace nytera;
static std::filesystem::path assets(const char* architecture, const char* filename)
{
    return std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build" / architecture / "Debug" / filename;
}
static Manifest fixtureBinding(const std::filesystem::path& source, Architecture architecture)
{
    Manifest manifest;
    manifest.sourceHash = sha256(readFile(source));
    manifest.sourceUniqueId = 0x4e795431;
    manifest.architecture = architecture;
    manifest.dllName = pathUtf8(source.filename());
    manifest.displayName = "Nytera Fixture";
    manifest.parameters = {{0, 0, "Gain", "", 0.5f, true}};
    manifest.inputs = {{"Input 1"}, {"Input 2"}};
    manifest.outputs = {{"Output 1"}, {"Output 2"}};
    return assignBindingIds(manifest);
}
TEST("Control.CrossBitness")
{
    auto source = assets("x86", "NyteraFixture.dll");
    BridgeClient client;
    require(client.start(fixtureBinding(source, Architecture::x86), assets("x86", "NyteraBridge.exe"), source));
    CHECK(client.processId() != 0);
    auto manifest = require(decodeManifest(require(client.request(ControlCommand::inspect))));
    CHECK(manifest.architecture == Architecture::x86);
    CHECK(manifest.displayName == "Nytera Fixture");
    auto process = OpenProcess(SYNCHRONIZE, FALSE, client.processId());
    CHECK(process);
    client.stop();
    CHECK(WaitForSingleObject(process, 3000) == WAIT_OBJECT_0);
    CloseHandle(process);
}
TEST("Control.VersionMismatch")
{
    auto bytes = encodeFrame({ControlCommand::inspect, 1, false, {std::byte{42}}});
    CHECK(bytes.size() == 25);
    bytes[4] ^= std::byte{255};
    CHECK(std::holds_alternative<Failure>(decodeFrame(bytes)));
}
TEST("Control.PartialFrame")
{
    auto bytes = encodeFrame({ControlCommand::inspect, 9, false, {std::byte{42}}});
    CHECK(require(decodeFrame(bytes)).payload == Bytes{std::byte{42}});
    for (size_t length = 0; length < bytes.size(); ++length)
        CHECK(std::holds_alternative<Failure>(decodeFrame(std::span(bytes).first(length))));
    bytes[16] = bytes[17] = bytes[18] = bytes[19] = std::byte{255};
    CHECK(std::holds_alternative<Failure>(decodeFrame(bytes)));
}
TEST("Control.StartupCallback")
{
    auto source = assets("x64", "NyteraFixture.dll");
    BridgeClient client;
    require(client.start(fixtureBinding(source, Architecture::x64), assets("x64", "NyteraBridge.exe"), source));
    CHECK(require(decodeManifest(require(client.request(ControlCommand::inspect)))).parameters.size() == 1);
}
TEST("Control.ParallelInstances")
{
    auto source = assets("x64", "NyteraFixture.dll");
    auto manifest = fixtureBinding(source, Architecture::x64);
    BridgeClient a, b;
    require(a.start(manifest, assets("x64", "NyteraBridge.exe"), source));
    require(b.start(manifest, assets("x64", "NyteraBridge.exe"), source));
    CHECK(a.processId() != b.processId());
    CHECK(a.endpoint() != b.endpoint());
    a.stop();
    CHECK(require(decodeManifest(require(b.request(ControlCommand::inspect)))).sourceUniqueId == 0x4e795431);
}
