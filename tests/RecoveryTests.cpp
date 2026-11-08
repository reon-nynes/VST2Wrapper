#include "bridge/BridgeClient.h"
#include "helper/Vst2Instance.h"
#include "common/Codec.h"
#include "common/Native.h"
#include "Test.h"
#include <thread>
#include <array>
using namespace nytera;
struct RecoverySession
{
    BridgeClient client;
    Manifest manifest;
    std::filesystem::path source, helper;
    RecoverySession()
    {
        auto folder = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build/x64/" NYTERA_TEST_CONFIG;
        source = folder / "NyteraStateFixture.dll"; helper = folder / "NyteraBridge.exe";
        auto native = require(Vst2Instance::open(source)); manifest = require(native->inspect());
        require(client.start(manifest, helper, source));
    }
    void command(uint32_t command)
    {
        HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(client.openEditor(reinterpret_cast<uint64_t>(parent)));
        SendMessageW(GetWindow(GetWindow(parent, GW_CHILD), GW_CHILD), WM_COMMAND, command, 0);
        client.closeEditor(); DestroyWindow(parent);
    }
};
TEST("Recovery.CrashDuringState")
{
    RecoverySession session;
    session.client.enqueueParameter({0, 0, 0.8f, 1});
    auto saved = require(session.client.captureState());
    session.command(109);
    CHECK(std::holds_alternative<Failure>(session.client.captureState(std::chrono::milliseconds(500))));
    CHECK(session.client.lastState() == saved);
    CHECK(session.client.status().phase == BridgePhase::failed);
    CHECK(!session.client.status().diagnostic.empty());
}
TEST("Recovery.HungHelper")
{
    RecoverySession session;
    auto saved = require(session.client.captureState());
    session.command(107);
    auto before = std::chrono::steady_clock::now();
    CHECK(std::holds_alternative<Failure>(session.client.captureState(std::chrono::milliseconds(80))));
    CHECK(session.client.status().phase == BridgePhase::failed);
    session.client.stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(700));
    CHECK(session.client.lastState() == saved);
}
TEST("Recovery.RetryBinding")
{
    RecoverySession session, healthy;
    session.client.enqueueParameter({0, 0, 0.8f, 1});
    auto saved = require(session.client.captureState());
    require(session.client.prepare({48000, 64, 2, 2, true}));
    auto old = session.client.processId();
    Handle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, old));
    CHECK(TerminateProcess(process.get(), 1));
    CHECK(WaitForSingleObject(process.get(), 1000) == WAIT_OBJECT_0);
    CHECK(session.client.status().phase == BridgePhase::failed);
    require(session.client.retry());
    CHECK(session.client.processId() != old && session.client.status().phase == BridgePhase::ready);
    CHECK(require(session.client.captureState()) == saved);
    CHECK(healthy.client.status().phase == BridgePhase::ready);
    auto wrong = session.manifest; wrong.conversionId[0] ^= std::byte{1};
    CHECK(std::holds_alternative<Failure>(session.client.start(wrong, session.helper, session.source)));
    std::array<float, 64> input, left, right; input.fill(1);
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {left.data(), right.data()};
    for (int block = 0; block < 5; ++block)
    {
        TransportSnapshot time; time.projectSamples = block * 64; time.flags = 2;
        CHECK(session.client.process(AudioBlockView{inputs, outputs, 64}, {}, time) != ProcessResult::failed);
        CHECK(left[0] == 0.8f);
    }
}
TEST("Recovery.TeardownRace")
{
    RecoverySession session;
    session.command(107);
    Handle process(OpenProcess(SYNCHRONIZE, FALSE, session.client.processId()));
    std::atomic<bool> entered {false};
    std::jthread pending([&] { entered = true; session.client.captureState(); });
    while (!entered) Sleep(1);
    Sleep(40);
    auto before = std::chrono::steady_clock::now();
    session.client.stop();
    pending.join();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(700));
    CHECK(WaitForSingleObject(process.get(), 1000) == WAIT_OBJECT_0);
}
TEST("Recovery.MalformedMessage")
{
    RecoverySession session;
    Writer malformed; malformed.number<uint32_t>(0xffffffff);
    CHECK(std::holds_alternative<Failure>(session.client.request(ControlCommand::parameter, malformed.bytes)));
    CHECK(session.client.status().phase == BridgePhase::failed);
    require(session.client.retry());
    CHECK(session.client.status().phase == BridgePhase::ready);
}
TEST("Recovery.EditorRace")
{
    RecoverySession session;
    session.command(110);
    Handle process(OpenProcess(SYNCHRONIZE, FALSE, session.client.processId()));
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    std::jthread pending([&] { session.client.openEditor(reinterpret_cast<uint64_t>(parent)); });
    Sleep(40);
    auto before = std::chrono::steady_clock::now();
    session.client.stop(); pending.join();
    DestroyWindow(parent);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(700));
    CHECK(WaitForSingleObject(process.get(), 1000) == WAIT_OBJECT_0);
}
TEST("Recovery.StartupCancel")
{
    RecoverySession session;
    session.client.stop();
    auto stalled = session.source.parent_path() / "NyteraStalledFixture.dll";
    auto binding = session.manifest;
    binding.sourceHash = sha256(readFile(stalled)); binding = assignBindingIds(binding);
    BridgeClient client;
    Result<std::monostate> result;
    std::jthread startup([&] { result = client.start(binding, session.helper, stalled); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.processId() == 0 && std::chrono::steady_clock::now() < deadline) Sleep(1);
    CHECK(client.processId() != 0);
    Handle process(OpenProcess(SYNCHRONIZE, FALSE, client.processId()));
    auto before = std::chrono::steady_clock::now();
    client.stop(); startup.join();
    CHECK(std::holds_alternative<Failure>(result));
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(700));
    CHECK(WaitForSingleObject(process.get(), 1000) == WAIT_OBJECT_0);
}
TEST("Recovery.OfflineStop")
{
    RecoverySession session;
    require(session.client.prepare({48000, 64, 2, 2, true}));
    session.command(112);
    std::atomic<bool> callbackEntered {false};
    std::jthread render([&]
    {
        std::array<float, 64> input {}, left {}, right {};
        std::array<const float*, 2> inputs {input.data(), input.data()};
        std::array<float*, 2> outputs {left.data(), right.data()};
        for (int block = 0; block < 3; ++block)
        {
            TransportSnapshot time; time.projectSamples = block * 64; time.flags = 2;
            callbackEntered = true;
            session.client.process(AudioBlockView{inputs, outputs, 64}, {}, time);
        }
    });
    while (!callbackEntered) Sleep(1);
    Sleep(40);
    auto before = std::chrono::steady_clock::now();
    session.client.stop(); render.join();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(700));
}
