#include "bridge/BridgeClient.h"
#include "helper/Vst2Instance.h"
#include "common/State.h"
#include "helper/AudioWorker.h"
#include "Test.h"
#include <algorithm>
#include <array>
#include <thread>
using namespace nytera;
struct StateSession
{
    Manifest manifest;
    std::filesystem::path source, helper;
    BridgeClient client;
    explicit StateSession(const std::string& variant = "NyteraStateFixture.dll", bool x86 = false)
    {
        auto root = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build";
        { auto native = require(Vst2Instance::open(root / "x64/" NYTERA_TEST_CONFIG / variant)); manifest = require(native->inspect()); }
        auto folder = root / (x86 ? "x86/" NYTERA_TEST_CONFIG : "x64/" NYTERA_TEST_CONFIG);
        source = folder / variant; helper = folder / "NyteraBridge.exe";
        manifest.sourceHash = sha256(readFile(source)); manifest.architecture = x86 ? Architecture::x86 : Architecture::x64;
        manifest = assignBindingIds(manifest);
        require(client.start(manifest, helper, source));
    }
};
TEST("State.ChunkExact")
{
    for (bool x86 : {false, true})
    {
        StateSession session("NyteraStateFixture.dll", x86);
        session.client.enqueueParameter({0, 0, 0.75f, 1});
        auto saved = require(session.client.captureState());
        CHECK(saved.kind == StateKind::bankChunk && saved.chunk.size() == 32);
        CHECK(saved.parameters == std::vector<float>{0.75f});
        CHECK(saved.chunk[12] == std::byte{0xa5});
        CHECK(require(decodeState(encodeState(saved))) == saved);
        session.client.enqueueParameter({0, 0, 0.25f, 1});
        CHECK(require(session.client.captureState()).chunk != saved.chunk);
        require(session.client.restoreState(saved));
        CHECK(require(session.client.captureState()) == saved);
    }
}
TEST("State.ParameterFallback")
{
    StateSession session("NyteraProgramFixture.dll");
    session.client.enqueueParameter({1, 0, 0, 3});
    session.client.enqueueParameter({0, 0, 0.8f, 1});
    auto saved = require(session.client.captureState());
    CHECK(saved.kind == StateKind::parameters && saved.chunk.empty() && saved.program == 1);
    CHECK(saved.parameters == std::vector<float>{0.8f});
    saved.programName = "Saved label";
    session.client.enqueueParameter({0, 0, 0, 3});
    session.client.enqueueParameter({0, 0, 0.25f, 1});
    CHECK(require(session.client.captureState()).parameters[0] == 0.25f);
    require(session.client.restoreState(saved));
    CHECK(require(session.client.captureState()) == saved);
}
TEST("State.BeforeStartup")
{
    StateSession session;
    session.client.enqueueParameter({0, 0, 0.8f, 1});
    auto saved = require(session.client.captureState());
    BridgeClient restored;
    require(restored.restoreState(saved));
    CHECK(restored.processId() == 0);
    require(restored.start(session.manifest, session.helper, session.source));
    require(restored.prepare({48000, 64, 2, 2, true}));
    std::array<float, 64> input, left, right;
    input.fill(1);
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {left.data(), right.data()};
    for (int block = 0; block < 5; ++block)
    {
        TransportSnapshot time; time.flags = 2; time.projectSamples = block * 64;
        CHECK(restored.process(AudioBlockView{inputs, outputs, 64}, {}, time) != ProcessResult::failed);
        CHECK(std::all_of(left.begin(), left.end(), [block](float value) { return value == (block < 2 ? 0.0f : 0.8f); }));
    }
}
TEST("State.CaptureTimeout")
{
    StateSession session;
    auto saved = require(session.client.captureState());
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(session.client.openEditor(reinterpret_cast<uint64_t>(parent)));
    SendMessageW(GetWindow(GetWindow(parent, GW_CHILD), GW_CHILD), WM_COMMAND, 107, 0);
    session.client.closeEditor(); DestroyWindow(parent);
    auto before = std::chrono::steady_clock::now();
    auto failed = session.client.captureState(std::chrono::milliseconds(50));
    CHECK(std::holds_alternative<Failure>(failed));
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
    CHECK(session.client.lastState() == saved);
}
TEST("State.IdentityMismatch")
{
    StateSession session;
    auto saved = require(session.client.captureState());
    auto wrong = saved; wrong.conversionId[0] ^= std::byte{1};
    auto result = session.client.restoreState(wrong);
    CHECK(std::holds_alternative<Failure>(result) && std::get<Failure>(result).code == ErrorCode::identity);
    CHECK(require(session.client.captureState()) == saved);
}
TEST("State.InstanceIsolation")
{
    StateSession first, second;
    first.client.enqueueParameter({0, 0, 0.2f, 1}); second.client.enqueueParameter({0, 0, 0.8f, 1});
    auto a = require(first.client.captureState()), b = require(second.client.captureState());
    CHECK(a.parameters[0] == 0.2f && b.parameters[0] == 0.8f);
    require(first.client.restoreState(b));
    CHECK(require(first.client.captureState()).parameters[0] == 0.8f);
    CHECK(require(second.client.captureState()) == b);
}
TEST("State.Truncated")
{
    StateEnvelope state; state.parameters = {0.5f};
    auto bytes = encodeState(state);
    for (size_t length = 0; length < bytes.size(); ++length)
        CHECK(std::holds_alternative<Failure>(decodeState(std::span(bytes).first(length))));
    bytes[20] ^= std::byte{1};
    CHECK(std::holds_alternative<Failure>(decodeState(bytes)));
}
TEST("State.QueuedGeneration")
{
    auto fixture = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraStateFixture.dll";
    auto source = require(Vst2Instance::open(fixture));
    ProcessConfig config {48000, 64, 2, 2, false, 1, 1};
    require(source->prepare(config));
    source->native()->set_parameter(source->native(), 0, 0.8f);
    auto saved = require(source->captureState());
    auto name = L"Local\\NyteraStateGeneration-" + std::to_wstring(GetCurrentProcessId());
    auto transport = require(AudioTransport::create(name, config));
    auto memory = require(SharedAudio::open(name, config));
    std::mutex sourceMutex;
    AudioWorker worker(std::move(memory), *source, sourceMutex);
    {
        std::unique_lock blockWorker(sourceMutex);
        std::array<float, 64> input {}, left {}, right {};
        std::array<const float*, 2> inputs {input.data(), input.data()};
        std::array<float*, 2> outputs {left.data(), right.data()};
        ParameterEvent old {0, 0, 0.25f, 1, 1};
        CHECK(transport->process(AudioBlockView{inputs, outputs, 64}, {{}, nullptr, nullptr, std::span(&old, 1)}, {}) != ProcessResult::failed);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (InterlockedCompareExchange(&transport->shared().slot(0).state, 0, 0) != 2)
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            Sleep(1);
        }
        transport->reset();
        require(source->restoreState(saved));
    }
    CHECK(WaitForSingleObject(transport->shared().doneEvent(), 1000) == WAIT_OBJECT_0);
    CHECK(source->native()->get_parameter(source->native(), 0) == 0.8f);
}
TEST("State.LargeSnapshot")
{
    StateSession session("NyteraLargeStateFixture.dll");
    CHECK(require(session.client.captureState()).parameters.size() == 9000);
    require(session.client.prepare({48000, 64, 2, 2, true}));
    session.client.resetProcessing();
    std::array<float, 64> input {}, left {}, right {};
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {left.data(), right.data()};
    CHECK(session.client.process(AudioBlockView{inputs, outputs, 64}, {}, {}) != ProcessResult::failed);
}
TEST("State.LegacyBinding")
{
    StateSession session("NyteraProgramFixture.dll");
    auto legacy = session.manifest; legacy.schemaVersion = 1; legacy.programs = {};
    BridgeClient client;
    require(client.start(legacy, session.helper, session.source));
    client.enqueueParameter({1, 0, 0, 3});
    client.enqueueParameter({0, 0, 0.8f, 1});
    auto saved = require(client.captureState());
    CHECK(saved.program == 1 && saved.parameters[0] == 0.8f);
    require(client.restoreState(saved));
    require(client.prepare({48000, 64, 2, 2, true}));
    CHECK(require(client.captureState()) == saved);
}
TEST("State.ConcurrentCapture")
{
    StateSession session;
    session.client.enqueueParameter({0, 0, 0.8f, 1});
    require(session.client.prepare({48000, 64, 2, 2, false}));
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(session.client.openEditor(reinterpret_cast<uint64_t>(parent)));
    SendMessageW(GetWindow(GetWindow(parent, GW_CHILD), GW_CHILD), WM_COMMAND, 108, 0);
    session.client.closeEditor(); DestroyWindow(parent);
    std::atomic<bool> done {false};
    Result<StateEnvelope> captured = Failure{ErrorCode::plugin, "Capture did not finish"};
    std::jthread snapshot([&] { captured = session.client.captureState(); done = true; });
    std::array<float, 64> input, left, right;
    input.fill(1);
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {left.data(), right.data()};
    uint32_t maintenanceBlocks = 0;
    bool heard = false;
    for (int block = 0; block < 250; ++block)
    {
        TransportSnapshot time; time.flags = 2; time.projectSamples = block * 64;
        auto before = std::chrono::steady_clock::now();
        auto result = session.client.process(AudioBlockView{inputs, outputs, 64}, {}, time);
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(50));
        CHECK(result != ProcessResult::failed);
        if (result == ProcessResult::priming && !done) ++maintenanceBlocks;
        if (done && left[0] == 0.8f) heard = true;
        Sleep(1);
    }
    snapshot.join();
    CHECK(require(captured).parameters[0] == 0.8f);
    CHECK(maintenanceBlocks >= 2 && heard);
}
