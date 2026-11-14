#include "common/Codec.h"
#include "bridge/BridgeClient.h"
#include "Test.h"
#include <windows.h>
#include <thread>
using namespace nytera;
static std::filesystem::path assets(const char* architecture, const char* filename)
{
    return std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build" / architecture / NYTERA_TEST_CONFIG / filename;
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
TEST("Control.EditorTracking")
{
    for (bool x86 : {false, true})
    {
        auto architecture = x86 ? "x86" : "x64";
        auto source = assets(architecture, "NyteraFixture.dll");
        BridgeClient client;
        require(client.start(fixtureBinding(source, x86 ? Architecture::x86 : Architecture::x64), assets(architecture, "NyteraBridge.exe"), source));
        require(client.prepare({48000, 64, 2, 2, false}));
        auto parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(parent);
        struct WindowCleanup { HWND window; ~WindowCleanup() { DestroyWindow(window); } } cleanup {parent};
        require(client.openEditor(reinterpret_cast<uint64_t>(parent)));
        auto original = GetWindow(GetWindow(parent, GW_CHILD), GW_CHILD);
        CHECK(original);
        struct EndTracking { HWND window; ~EndTracking() { RemovePropW(window, L"NyteraFixtureTracking"); } } endTracking {original};
        CHECK(PostMessageW(original, WM_COMMAND, 115, 0));
        auto deadline = GetTickCount64() + 2000;
        while (!GetPropW(original, L"NyteraFixtureTracking") && GetTickCount64() < deadline)
        {
            MSG message;
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
            Sleep(1);
        }
        CHECK(GetPropW(original, L"NyteraFixtureTracking"));
        // This is the same notification request that used to fail during D82 drags.
        require(client.request(ControlCommand::poll, {}, std::chrono::milliseconds(200)));
        CHECK(GetPropW(original, L"NyteraFixtureTracking"));
        CHECK(require(client.parameterDisplay(0)).text == "0.50");
        CHECK(PostMessageW(original, WM_COMMAND, 116, 0));
        bool synchronized = false;
        deadline = GetTickCount64() + 500;
        while (!synchronized && GetTickCount64() < deadline)
            for (const auto& event : require(client.takeNotifications()))
                if (event.opcode == 0 && event.index == 0 && event.normalized == 0.75f) synchronized = true;
        CHECK(synchronized && GetPropW(original, L"NyteraFixtureTracking"));
        CHECK(require(client.parameterValues())[0] == 0.75f);
        // Host automation must still reach the source inside the tracking loop.
        client.enqueueParameter({0, 0, 0.5f, 1});
        require(client.flushParameters());
        CHECK(require(client.parameterValues())[0] == 0.5f);
        std::array<float, 64> input, left, right;
        input.fill(0.8f);
        std::array<const float*, 2> inputs {input.data(), input.data()};
        std::array<float*, 2> outputs {left.data(), right.data()};
        for (int block = 0; block < 10; ++block)
        {
            TransportSnapshot transport; transport.projectSamples = block * 64; transport.flags = 2;
            CHECK(client.process(AudioBlockView{inputs, outputs, 64}, {}, transport) == ProcessResult::rendered);
            CHECK(left[0] == 0.4f && right[0] == 0.4f);
            require(client.takeNotifications());
        }
        // Closing must wait for the tracking handler to return, rather than
        // destroying its editor reentrantly from audioMasterIdle.
        std::atomic<bool> closed {false};
        std::jthread closer([&] { client.closeEditor(); closed = true; });
        Sleep(30);
        const bool stillTracking = GetPropW(original, L"NyteraFixtureTracking") && !closed.load();
        RemovePropW(original, L"NyteraFixtureTracking");
        // Destroying a cross-process child synchronously notifies this parent.
        // Keep its owner thread responsive while the close request completes.
        deadline = GetTickCount64() + 2500;
        while (!closed.load() && GetTickCount64() < deadline)
        {
            MSG message;
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
            Sleep(1);
        }
        closer.join();
        CHECK(stillTracking && closed.load());
        CHECK(client.status().phase == BridgePhase::ready);
    }
}
