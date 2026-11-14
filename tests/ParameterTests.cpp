#include "bridge/BridgeClient.h"
#include "helper/Vst2Instance.h"
#include "Test.h"
#include <array>
#include "Vst3Host.h"
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <thread>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <cmath>
#include "common/BoundedQueue.h"
#include "common/Native.h"
namespace Steinberg { DEF_CLASS_IID(IPlugViewContentScaleSupport) }
using namespace nytera;
struct StateOutputChanges : Steinberg::Vst::IParameterChanges
{
    int count = 0;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::int32 PLUGIN_API getParameterCount() override { return count; }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32) override { return nullptr; }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID&, Steinberg::int32&) override { ++count; return nullptr; }
};
struct ParameterSession
{
    BridgeClient client;
    ParameterSession()
    {
        auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
        auto manifest = require(instance->inspect());
        auto helper = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraBridge.exe";
        require(client.start(manifest, helper, NYTERA_FIXTURE));
    }
};
TEST("Parameter.NoEcho")
{
    ParameterSession session;
    require(session.client.prepare({48000, 64, 2, 2, true}));
    session.client.enqueueParameter({0, 0, 0.75f, 1});
    std::array<float, 64> input, output;
    input.fill(1);
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {output.data(), output.data()};
    for (int i = 0; i < 5; ++i)
    {
        TransportSnapshot time;
        time.projectSamples = i * 64; time.flags = 2;
        CHECK(session.client.process(AudioBlockView{inputs, outputs, 64}, {}, time) != ProcessResult::failed);
    }
    CHECK(output[0] == 0.75f);
    auto notifications = require(session.client.takeNotifications());
    for (const auto& event : notifications) CHECK(event.opcode != 0);
}
TEST("Parameter.Display")
{
    ParameterSession session;
    session.client.enqueueParameter({0, 0, 0.75f, 1});
    require(session.client.flushParameters());
    CHECK(require(session.client.parameterDisplay(0)).text == "0.75");
    CHECK(std::holds_alternative<Failure>(session.client.parameterDisplay(1000)));
}
TEST("Parameter.BlockBoundary")
{
    ParameterSession session;
    require(session.client.prepare({48000, 64, 2, 2, true}));
    std::array<float, 32> input, left, right;
    input.fill(1);
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {left.data(), right.data()};
    for (int block = 0; block < 10; ++block)
    {
        if (block < 2) session.client.enqueueParameter({0, 0, block == 0 ? 0.25f : 0.75f, 1});
        TransportSnapshot time; time.projectSamples = block * 32; time.flags = 2;
        CHECK(session.client.process(AudioBlockView{inputs, outputs, 32}, {}, time) != ProcessResult::failed);
        float expected = block == 0 ? 0.25f : 0.75f;
        CHECK(std::all_of(left.begin(), left.end(), [expected](float value) { return value == expected; }));
    }
}
struct ParameterEditor
{
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND original = nullptr;
    explicit ParameterEditor(BridgeClient& client)
    {
        require(client.openEditor(reinterpret_cast<uint64_t>(parent)));
        original = GetWindow(GetWindow(parent, GW_CHILD), GW_CHILD);
        CHECK(original != nullptr);
    }
    ~ParameterEditor() { DestroyWindow(parent); }
};
TEST("Parameter.Gestures")
{
    ParameterSession session;
    ParameterEditor editor(session.client);
    SendMessageW(editor.original, WM_LBUTTONDOWN, 0, 0);
    auto events = require(session.client.takeNotifications());
    CHECK(events.size() == 3);
    CHECK(events[0].opcode == 43 && events[1].opcode == 0 && events[2].opcode == 44);
    CHECK(events[1].index == 0 && events[1].normalized == 0.75f);
    session.client.closeEditor();
}
TEST("Parameter.StaleEditor")
{
    ParameterSession session;
    require(session.client.prepare({48000, 64, 2, 2, true}));
    ParameterEditor editor(session.client);
    session.client.enqueueParameter({0, 0, 0.8f, 1});
    SendMessageW(editor.original, WM_COMMAND, 113, 0);
    auto events = require(session.client.takeNotifications());
    CHECK(std::none_of(events.begin(), events.end(), [](const auto& event) { return event.opcode == 0; }));
    CHECK(require(session.client.parameterValues())[0] == 0.8f);
    require(session.client.flushParameters());
    CHECK(require(session.client.parameterValues())[0] == 0.8f);
    session.client.closeEditor();
}
TEST("Parameter.EditorPreservesValues")
{
    ParameterSession session;
    ParameterEditor editor(session.client);
    SendMessageW(editor.original, WM_COMMAND, 114, 0);
    session.client.closeEditor();
    session.client.enqueueParameter({0, 0, 0.8f, 1}); require(session.client.flushParameters());
    require(session.client.openEditor(reinterpret_cast<uint64_t>(editor.parent)));
    CHECK(require(session.client.parameterValues())[0] == 0.8f);
    auto events = require(session.client.takeNotifications());
    CHECK(std::none_of(events.begin(), events.end(), [](const auto& event) { return event.opcode == 0; }));
    session.client.closeEditor();
}
TEST("Parameter.TopologyChange")
{
    ParameterSession session;
    ParameterEditor editor(session.client);
    SendMessageW(editor.original, WM_COMMAND, 103, 0);
    CHECK(std::holds_alternative<Failure>(session.client.request(ControlCommand::poll)));
    session.client.closeEditor();
}
TEST("Parameter.Latency")
{
    ParameterSession session;
    require(session.client.prepare({48000, 64, 2, 2, true}));
    ParameterEditor editor(session.client);
    SendMessageW(editor.original, WM_COMMAND, 104, 0);
    auto events = require(session.client.takeNotifications());
    CHECK(events.size() == 1 && events[0].opcode == 13 && events[0].value == 32);
    CHECK(session.client.latencySamples() == 32);
    session.client.closeEditor();
}
TEST("Parameter.CloseGesture")
{
    ParameterSession session;
    ParameterEditor editor(session.client);
    SendMessageW(editor.original, WM_COMMAND, 106, 0);
    session.client.closeEditor();
    auto events = require(session.client.takeNotifications());
    CHECK(events.size() == 3);
    CHECK(events[0].opcode == 43 && events[1].opcode == 0 && events[2].opcode == 44);
}
TEST("Parameter.HostInfo")
{
    auto source = require(Vst2Instance::open(NYTERA_FIXTURE));
    require(source->prepare({48000, 64, 2, 2, false}));
    std::array<float, 64> input {}, left {}, right {};
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {left.data(), right.data()};
    TransportSnapshot transport;
    transport.flags = 2 | 1024; transport.tempo = 123; transport.projectSamples = 2048;
    require(source->process({inputs, outputs, 64}, {}, transport));
    CHECK(source->dispatch(50, 1) == 123);
    CHECK(source->dispatch(50, 2) == 2048);
}
TEST("Parameter.Ordering")
{
    auto source = require(Vst2Instance::open(NYTERA_FIXTURE));
    require(source->applyParameter({0, 0, 0.75f, 1, 2}));
    require(source->applyParameter({0, 0, 0.25f, 1, 1}));
    CHECK(source->native()->get_parameter(source->native(), 0) == 0.75f);
}
TEST("Parameter.CallbackQueue")
{
    auto queue = std::make_unique<BoundedQueue<uint32_t, 1024>>();
    std::vector<std::jthread> producers;
    for (uint32_t producer = 0; producer < 4; ++producer)
        producers.emplace_back([&, producer](std::stop_token stop)
        {
            for (uint32_t index = 0; index < 1000 && !stop.stop_requested(); ++index)
                while (!queue->push(producer * 1000 + index))
                    if (stop.stop_requested()) return;
        });
    std::array<uint32_t, 4> expected {};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (uint32_t count = 0; count < 4000;)
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        uint32_t value;
        if (!queue->pop(value)) { std::this_thread::yield(); continue; }
        CHECK(value / 1000 < 4 && expected[value / 1000]++ == value % 1000);
        ++count;
    }
}
using namespace Steinberg;
using namespace Steinberg::Vst;
struct RecordingHandler : IComponentHandler
{
    uint32 references = 1;
    uint32 begins = 0, ends = 0;
    std::vector<std::pair<ParamID, double>> values;
    std::function<void()> onEdit;
    tresult PLUGIN_API queryInterface(const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid))
        { *object = static_cast<IComponentHandler*>(this); addRef(); return kResultOk; }
        *object = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++references; }
    uint32 PLUGIN_API release() override { return --references; }
    tresult PLUGIN_API beginEdit(ParamID) override { ++begins; return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { ++ends; return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override
    { values.push_back({id, value}); if (onEdit) onEdit(); return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};
static void pumpMessages()
{
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    do
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < until);
}
struct ParameterPlugin
{
    FactoryModule module;
    RecordingHandler handler;
    IComponent* component = nullptr;
    IEditController* controller = nullptr;
    IConnectionPoint* sourcePoint = nullptr;
    IConnectionPoint* controllerPoint = nullptr;
    IPlugView* view = nullptr;
    HWND window = nullptr;
    explicit ParameterPlugin(const std::filesystem::path& path) : module(path)
    {
        auto info = module.component();
        CHECK(module.factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
        CHECK(component->initialize(nullptr) == kResultOk);
        TUID controllerId;
        CHECK(component->getControllerClassId(controllerId) == kResultOk);
        CHECK(module.factory->createInstance(controllerId, IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
        CHECK(controller->initialize(nullptr) == kResultOk);
        CHECK(controller->setComponentHandler(&handler) == kResultOk);
        CHECK(component->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&sourcePoint)) == kResultOk);
        CHECK(controller->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&controllerPoint)) == kResultOk);
        CHECK(sourcePoint->connect(controllerPoint) == kResultOk);
        CHECK(controllerPoint->connect(sourcePoint) == kResultOk);
    }
    ~ParameterPlugin()
    {
        if (view) { view->removed(); view->release(); }
        if (window) DestroyWindow(window);
        if (controllerPoint) controllerPoint->disconnect(sourcePoint);
        if (sourcePoint) sourcePoint->disconnect(controllerPoint);
        if (controllerPoint) controllerPoint->release();
        if (sourcePoint) sourcePoint->release();
        if (controller) { controller->setComponentHandler(nullptr); controller->terminate(); controller->release(); }
        if (component) { component->setActive(false); component->terminate(); component->release(); }
    }
    void openEditor()
    {
        window = CreateWindowExW(0, L"STATIC", L"Nytera parameter integration test", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        view = controller->createView(ViewType::kEditor);
        CHECK(view && view->attached(window, kPlatformTypeHWND) == kResultOk);
        pumpMessages();
    }
};
static void parameterThroughVst3(bool captureDuringEdit)
{
    auto folder = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/parameter-tests" / std::to_string(GetCurrentProcessId());
    std::filesystem::create_directories(folder);
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    auto manifest = require(instance->inspect());
    auto helper = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraBridge.exe";
    auto path = folder / "Parameter.vst3";
    require(writePackage({manifest, NYTERA_EFFECT_TEMPLATE, helper, NYTERA_FIXTURE, {}}, path));
    ParameterPlugin plugin(path);
    plugin.openEditor();
    auto id = manifest.parameters[0].vst3Id;
    plugin.controller->setParamNormalized(id, 0.25);
    pumpMessages();
    String128 text {};
    CHECK(plugin.controller->getParamStringByValue(id, 0.25, text) == kResultOk);
    CHECK(std::u16string(reinterpret_cast<const char16_t*>(text)) == u"0.25");
    HWND original = nullptr;
    EnumChildWindows(plugin.window, [](HWND window, LPARAM context) -> BOOL
    {
        wchar_t name[128]; GetClassNameW(window, name, 128);
        if (std::wstring(name) == L"NyteraFixtureEditor") { *reinterpret_cast<HWND*>(context) = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&original));
    CHECK(original != nullptr);
    CHECK(plugin.handler.begins == 0 && plugin.handler.ends == 0);
    bool captured = false;
    if (captureDuringEdit) plugin.handler.onEdit = [&]
    {
        TestStateStream state;
        captured = plugin.component->getState(&state) == kResultOk && state.bytes.size() > 100;
    };
    SendMessageW(original, WM_LBUTTONDOWN, 0, 0);
    pumpMessages();
    CHECK(plugin.handler.begins == 1 && plugin.handler.ends == 1);
    CHECK(plugin.controller->getParamNormalized(id) == 0.5);
    CHECK(plugin.handler.values.size() == 1 && plugin.handler.values[0].first == id && plugin.handler.values[0].second == 0.5);
    if (captureDuringEdit) CHECK(captured);
}
TEST("Parameter.Vst3Entry") { parameterThroughVst3(false); }
TEST("Parameter.EditorTracking")
{
    auto folder = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/parameter-tests" / std::to_string(GetCurrentProcessId());
    std::filesystem::create_directories(folder);
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    auto manifest = require(instance->inspect());
    auto helper = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraBridge.exe";
    auto path = folder / "Tracking.vst3";
    require(writePackage({manifest, NYTERA_EFFECT_TEMPLATE, helper, NYTERA_FIXTURE, {}}, path));
    ParameterPlugin plugin(path);
    plugin.openEditor();
    HWND original = nullptr;
    EnumChildWindows(plugin.window, [](HWND window, LPARAM context) -> BOOL
    {
        wchar_t name[128] {}; GetClassNameW(window, name, 128);
        if (std::wstring(name) == L"NyteraFixtureEditor") *reinterpret_cast<HWND*>(context) = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&original));
    CHECK(original);
    struct EndTracking { HWND window; ~EndTracking() { RemovePropW(window, L"NyteraFixtureTracking"); } } endTracking {original};
    CHECK(PostMessageW(original, WM_COMMAND, 115, 0));
    pumpMessages();
    CHECK(GetPropW(original, L"NyteraFixtureTracking"));
    CHECK(plugin.handler.begins == 1 && plugin.handler.ends == 0);
    auto id = manifest.parameters[0].vst3Id;
    for (double value : {0.75, 0.25})
    {
        CHECK(PostMessageW(original, WM_COMMAND, 116, 0));
        pumpMessages();
        CHECK(GetPropW(original, L"NyteraFixtureTracking"));
        CHECK(plugin.controller->getParamNormalized(id) == value);
        CHECK(!plugin.handler.values.empty());
        CHECK(plugin.handler.values.back().first == id && plugin.handler.values.back().second == value);
        CHECK(plugin.handler.begins == 1 && plugin.handler.ends == 0);
    }
    CHECK(plugin.controller->setParamNormalized(id, 0.6) == kResultOk);
    pumpMessages();
    String128 display {};
    CHECK(plugin.controller->getParamStringByValue(id, 0.6, display) == kResultOk);
    CHECK(std::u16string(reinterpret_cast<const char16_t*>(display)) == u"0.60");
    CHECK(GetPropW(original, L"NyteraFixtureTracking"));
    RemovePropW(original, L"NyteraFixtureTracking");
    pumpMessages();
    CHECK(plugin.handler.begins == 1 && plugin.handler.ends == 1);
}
TEST("State.ReentrantSnapshot") { parameterThroughVst3(true); }
static void stateThroughVst3(bool programs)
{
    auto root = std::filesystem::path(NYTERA_SOURCE_ROOT);
    auto folder = root / "out/state-vst3-tests" / std::to_string(GetCurrentProcessId());
    std::filesystem::create_directories(folder);
    auto source = root / "out/build/x64/" NYTERA_TEST_CONFIG / (programs ? "NyteraProgramFixture.dll" : "NyteraStateFixture.dll");
    auto instance = require(Vst2Instance::open(source));
    auto manifest = require(instance->inspect());
    auto path = folder / "State.vst3";
    require(writePackage({manifest, NYTERA_EFFECT_TEMPLATE, root / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraBridge.exe", source, {}}, path));
    ParameterPlugin first(path);
    auto id = manifest.parameters[0].vst3Id;
    ParamID programId = 0;
    if (programs)
    {
        for (int32 index = 0; index < first.controller->getParameterCount(); ++index)
        {
            Steinberg::Vst::ParameterInfo info;
            CHECK(first.controller->getParameterInfo(index, info) == kResultOk);
            if (info.flags & Steinberg::Vst::ParameterInfo::kIsProgramChange) programId = info.id;
        }
        CHECK(programId != 0);
        CHECK(first.controller->setParamNormalized(programId, 1.0) == kResultOk);
    }
    CHECK(first.controller->setParamNormalized(id, 0.8) == kResultOk);
    TestStateStream saved;
    CHECK(first.component->getState(&saved) == kResultOk);
    ParameterPlugin restored(path);
    saved.position = 0;
    CHECK(restored.component->setState(&saved) == kResultOk);
    saved.position = 0;
    CHECK(restored.controller->setComponentState(&saved) == kResultOk);
    CHECK(std::abs(restored.controller->getParamNormalized(id) - 0.8) < 0.000001);
    if (programs) CHECK(restored.controller->getParamNormalized(programId) == 1.0);
    CHECK(restored.handler.begins == 0 && restored.handler.ends == 0 && restored.handler.values.empty());
    IAudioProcessor* raw = nullptr;
    CHECK(restored.component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&raw)) == kResultOk);
    auto release = [&](IAudioProcessor* processor) { processor->setProcessing(false); restored.component->setActive(false); processor->release(); };
    std::unique_ptr<IAudioProcessor, decltype(release)> processor(raw, release);
    ProcessSetup setup {kOffline, kSample32, 64, 48000};
    CHECK(processor->setupProcessing(setup) == kResultOk);
    restored.component->activateBus(kAudio, kInput, 0, true);
    restored.component->activateBus(kAudio, kOutput, 0, true);
    CHECK(restored.component->setActive(true) == kResultOk);
    processor->setProcessing(true);
    std::array<float, 64> input, left, right;
    input.fill(1);
    std::array<float*, 2> inputs {input.data(), input.data()}, outputs {left.data(), right.data()};
    AudioBusBuffers in {}, out {};
    in.numChannels = out.numChannels = 2; in.channelBuffers32 = inputs.data(); out.channelBuffers32 = outputs.data();
    ProcessContext context {}; context.sampleRate = 48000; context.state = ProcessContext::kPlaying;
    ProcessData data;
    data.processMode = kOffline; data.symbolicSampleSize = kSample32; data.numSamples = 64;
    data.numInputs = data.numOutputs = 1; data.inputs = &in; data.outputs = &out; data.processContext = &context;
    for (int block = 0; block < 5; ++block)
    {
        context.projectTimeSamples = block * 64;
        CHECK(processor->process(data) == kResultOk);
        for (float value : left) CHECK(value == 0.8f);
    }
    context.state = 0;
    CHECK(processor->process(data) == kResultOk);
    CHECK(restored.controller->setParamNormalized(id, 0.125) == kResultOk);
    TestStateStream changed;
    CHECK(restored.component->getState(&changed) == kResultOk);
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        auto& next = cycle == 1 ? changed : saved;
        next.position = 0;
        CHECK(restored.component->setState(&next) == kResultOk);
        CHECK(std::abs(restored.controller->getParamNormalized(id) - (cycle == 1 ? 0.125 : 0.8)) < 0.000001);
        next.position = 0;
        CHECK(restored.controller->setComponentState(&next) == kResultOk);
        TestStateStream captured;
        CHECK(restored.component->getState(&captured) == kResultOk);
        CHECK(captured.bytes == next.bytes);
    }
    context.state = 0;
    CHECK(processor->process(data) == kResultOk);
    std::jthread hostEdit([&] { restored.controller->setParamNormalized(id, 0.125); });
    hostEdit.join();
    saved.position = 0;
    CHECK(restored.component->setState(&saved) == kResultOk);
    saved.position = 0;
    CHECK(restored.controller->setComponentState(&saved) == kResultOk);
    StateOutputChanges stale;
    ProcessData flush;
    flush.outputParameterChanges = &stale;
    CHECK(processor->process(flush) == kResultOk);
    CHECK(stale.count == 0);
}
TEST("State.Vst3Entry") { stateThroughVst3(false); }
TEST("State.Vst3Programs") { stateThroughVst3(true); }
TEST("Editor.ContentScale")
{
    auto root = std::filesystem::path(NYTERA_SOURCE_ROOT);
    auto folder = root / "out/scale-tests" / std::to_string(GetCurrentProcessId());
    std::filesystem::create_directories(folder);
    auto source = std::filesystem::path(NYTERA_FIXTURE);
    auto native = require(Vst2Instance::open(source)); auto manifest = require(native->inspect());
    auto path = folder / "Scale.vst3";
    require(writePackage({manifest, NYTERA_EFFECT_TEMPLATE, root / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraBridge.exe", source, {}}, path));
    ParameterPlugin plugin(path); plugin.openEditor();
    struct Frame : IPlugFrame
    {
        ParameterPlugin& plugin;
        explicit Frame(ParameterPlugin& plugin) : plugin(plugin) { plugin.view->setFrame(this); }
        ~Frame() { plugin.view->setFrame(nullptr); }
        tresult PLUGIN_API queryInterface(const TUID, void**) override { return kNoInterface; }
        uint32 PLUGIN_API addRef() override { return 1; }
        uint32 PLUGIN_API release() override { return 1; }
        tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override
        {
            SetWindowPos(plugin.window, nullptr, 0, 0, size->getWidth(), size->getHeight(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return view->onSize(size);
        }
    } frame(plugin);
    HWND original = nullptr;
    EnumChildWindows(plugin.window, [](HWND window, LPARAM result) -> BOOL
    {
        wchar_t type[128] {}; GetClassNameW(window, type, 128);
        if (std::wstring(type) == L"NyteraFixtureEditor") *reinterpret_cast<HWND*>(result) = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&original));
    CHECK(original);
    IPlugViewContentScaleSupport* raw = nullptr;
    CHECK(plugin.view->queryInterface(IPlugViewContentScaleSupport::iid, reinterpret_cast<void**>(&raw)) == kResultOk);
    auto release = [](IPlugViewContentScaleSupport* support) { support->release(); };
    std::unique_ptr<IPlugViewContentScaleSupport, decltype(release)> support(raw, release);
    for (float factor : {1.0f, 1.5f, 2.0f, 1.0f})
    {
        CHECK(support->setContentScaleFactor(factor) == kResultOk); pumpMessages();
        RECT editor {}, parent {}; GetWindowRect(original, &editor); GetWindowRect(plugin.window, &parent);
        CHECK(editor.left >= parent.left && editor.top >= parent.top && editor.right <= parent.right + 1 && editor.bottom <= parent.bottom + 1);
    }
}
struct MidiPlugin : ParameterPlugin
{
    IAudioProcessor* audio = nullptr;
    std::array<float, 64> left {}, right {};
    std::array<float*, 2> channels {left.data(), right.data()};
    AudioBusBuffers output {};
    ProcessContext context {};
    ProcessData data;
    TestEventList incoming, outgoing;
    int64_t position = 0;
    static std::filesystem::path package(const char* name)
    {
        auto root = std::filesystem::path(NYTERA_SOURCE_ROOT);
        auto folder = root / "out/midi-integration" / (std::string(name) + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(folder);
        auto source = root / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraInstrumentFixture.dll";
        auto native = require(Vst2Instance::open(source)); auto manifest = require(native->inspect());
        auto path = folder / "Midi.vst3";
        require(writePackage({manifest, NYTERA_INSTRUMENT_TEMPLATE, root / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraBridge.exe", source, {}}, path));
        return path;
    }
    explicit MidiPlugin(const char* name) : ParameterPlugin(package(name))
    {
        CHECK(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&audio)) == kResultOk);
        ProcessSetup setup {kOffline, kSample32, 64, 48000};
        CHECK(audio->setupProcessing(setup) == kResultOk);
        CHECK(component->activateBus(kAudio, kOutput, 0, true) == kResultOk);
        CHECK(component->activateBus(kEvent, kInput, 0, true) == kResultOk);
        CHECK(component->activateBus(kEvent, kOutput, 0, true) == kResultOk);
        CHECK(component->setActive(true) == kResultOk); CHECK(audio->setProcessing(true) == kResultOk);
        outgoing.events.reserve(8192);
        output.numChannels = 2; output.channelBuffers32 = channels.data();
        context.sampleRate = 48000; context.state = ProcessContext::kPlaying;
        data.processMode = kOffline; data.symbolicSampleSize = kSample32; data.numSamples = 64;
        data.numOutputs = 1; data.outputs = &output; data.processContext = &context;
        data.inputEvents = &incoming; data.outputEvents = &outgoing;
    }
    ~MidiPlugin() { audio->setProcessing(false); component->setActive(false); audio->release(); }
    tresult process()
    {
        context.projectTimeSamples = position; outgoing.events.clear();
        auto result = audio->process(data); position += data.numSamples; return result;
    }
    HWND originalEditor() const
    {
        HWND result = nullptr;
        EnumChildWindows(window, [](HWND child, LPARAM target) -> BOOL
        {
            wchar_t name[128] {}; GetClassNameW(child, name, 128);
            if (std::wstring(name) == L"NyteraFixtureEditor") *reinterpret_cast<HWND*>(target) = child;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        return result;
    }
};
TEST("Recovery.MidiTimeline")
{
    MidiPlugin plugin("retry"); plugin.openEditor();
    Event note {}; note.type = Event::kNoteOnEvent; note.sampleOffset = 17;
    note.noteOn.channel = 0; note.noteOn.pitch = 60; note.noteOn.velocity = 1;
    for (int block = 0; block < 6; ++block)
    {
        plugin.incoming.events = block == 0 ? std::vector<Event>{note} : std::vector<Event>{};
        CHECK(plugin.process() == kResultOk);
    }
    CHECK(plugin.left.back() == 0.5f);
    HWND original = plugin.originalEditor(); CHECK(original);
    RECT originalBounds {}; GetWindowRect(original, &originalBounds);
    DWORD oldProcess = 0; GetWindowThreadProcessId(original, &oldProcess);
    Handle helper(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, oldProcess));
    CHECK(TerminateProcess(helper.get(), 1)); CHECK(WaitForSingleObject(helper.get(), 1000) == WAIT_OBJECT_0);
    pumpMessages();
    plugin.data.numSamples = 0;
    CHECK(plugin.process() == kResultOk && plugin.outgoing.events.empty());
    plugin.data.numSamples = 64;
    CHECK(plugin.process() == kResultOk && plugin.outgoing.events.size() == 16);
    for (const auto& event : plugin.outgoing.events)
        CHECK(event.type == Event::kLegacyMIDICCOutEvent && event.midiCCOut.controlNumber == 123);
    std::unique_ptr<HWND__, decltype(&DestroyWindow)> overlap(CreateWindowExW(WS_EX_TOPMOST, L"BUTTON",
        L"Nytera test input overlap", WS_POPUP | WS_VISIBLE, originalBounds.left, originalBounds.top,
        originalBounds.right - originalBounds.left, originalBounds.bottom - originalBounds.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr), &DestroyWindow);
    CHECK(overlap);
    CHECK(SetWindowPos(plugin.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW));
    SetForegroundWindow(plugin.window);
    HWND peer = GetWindow(plugin.window, GW_CHILD); CHECK(peer);
    SetFocus(peer);
    POINT point {(originalBounds.left + originalBounds.right) / 2, originalBounds.bottom - MulDiv(31, GetDpiForWindow(peer), 96)};
    ScreenToClient(peer, &point);
    POINT screen = point; ClientToScreen(peer, &screen); SetCursorPos(screen.x, screen.y);
    CHECK(WindowFromPoint(screen) == peer);
    auto coordinate = MAKELPARAM(point.x, point.y);
    SendMessageW(peer, WM_MOUSEMOVE, 0, coordinate);
    SendMessageW(peer, WM_LBUTTONDOWN, MK_LBUTTON, coordinate); SendMessageW(peer, WM_LBUTTONUP, 0, coordinate);
    pumpMessages();
    original = plugin.originalEditor(); CHECK(original);
    DWORD newProcess = 0; GetWindowThreadProcessId(original, &newProcess); CHECK(newProcess != oldProcess);
    bool echoed = false;
    for (int block = 0; block < 6; ++block)
    {
        plugin.incoming.events = block == 0 ? std::vector<Event>{note} : std::vector<Event>{};
        CHECK(plugin.process() == kResultOk);
        if (block >= 2) CHECK(plugin.left.back() == 0.5f);
        for (const auto& event : plugin.outgoing.events)
            if (event.type == Event::kNoteOnEvent)
            { CHECK(event.sampleOffset == 17 && event.noteOn.pitch == 60); echoed = true; }
    }
    CHECK(echoed);
}
struct MidiPoints : IParamValueQueue
{
    ParamID id = 0;
    int32 count = 0, offset = 0;
    tresult PLUGIN_API queryInterface(const TUID, void** object) override { *object = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 index, int32& sample, ParamValue& value) override
    { if (index < 0 || index >= count) return kInvalidArgument; sample = offset; value = 0.25; return kResultOk; }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kNotImplemented; }
};
struct MidiChanges : IParameterChanges
{
    MidiPoints points;
    explicit MidiChanges(MidiPlugin& plugin)
    {
        IMidiMapping* mapping = nullptr;
        CHECK(plugin.controller->queryInterface(IMidiMapping::iid, reinterpret_cast<void**>(&mapping)) == kResultOk);
        auto result = mapping->getMidiControllerAssignment(0, 0, 7, points.id); mapping->release();
        CHECK(result == kResultOk);
    }
    tresult PLUGIN_API queryInterface(const TUID, void** object) override { *object = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override { return index == 0 ? &points : nullptr; }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override { return nullptr; }
};
TEST("Processor.SysexBudget")
{
    MidiPlugin plugin("sysex-budget");
    Bytes bytes(1024 * 1024, std::byte{0x42}); bytes.front() = std::byte{0xf0}; bytes.back() = std::byte{0xf7};
    Event sysex {}; sysex.type = Event::kDataEvent; sysex.sampleOffset = 17;
    sysex.data.type = DataEvent::kMidiSysEx; sysex.data.bytes = reinterpret_cast<const uint8*>(bytes.data());
    sysex.data.size = static_cast<uint32>(bytes.size()); plugin.incoming.events.assign(3, sysex);
    CHECK(plugin.process() == kInvalidArgument);
    CHECK(std::all_of(plugin.left.begin(), plugin.left.end(), [](float value) { return value == 0; }));
    CHECK(plugin.outgoing.events.empty());
    plugin.incoming.events.assign(1, sysex);
    bool echoed = false;
    for (int block = 0; block < 5; ++block)
    {
        CHECK(plugin.process() == kResultOk); plugin.incoming.events.clear();
        for (const auto& event : plugin.outgoing.events)
            if (event.type == Event::kDataEvent)
            { CHECK(event.data.size == bytes.size() && event.sampleOffset == 17); echoed = true; }
    }
    CHECK(echoed);
}
TEST("Processor.ControllerBudget")
{
    MidiPlugin plugin("controller-budget"); MidiChanges changes(plugin);
    plugin.data.inputParameterChanges = &changes;
    changes.points.count = 4097;
    CHECK(plugin.process() == kInvalidArgument);
    changes.points.count = 2;
    Event note {}; note.type = Event::kNoteOnEvent; note.noteOn.pitch = 60; note.noteOn.velocity = 1;
    plugin.incoming.events.assign(4095, note);
    CHECK(plugin.process() == kInvalidArgument);
    changes.points.count = 4096; plugin.incoming.events.clear();
    CHECK(plugin.process() == kResultOk);
}
TEST("Processor.MidiOrdering")
{
    MidiPlugin plugin("midi-ordering"); MidiChanges changes(plugin);
    changes.points.count = 1; changes.points.offset = 17; plugin.data.inputParameterChanges = &changes;
    std::array<uint8, 5> bytes {0xf0, 1, 2, 3, 0xf7};
    Event note {}; note.type = Event::kNoteOnEvent; note.sampleOffset = 17;
    note.noteOn.pitch = 60; note.noteOn.velocity = 1;
    Event sysex {}; sysex.type = Event::kDataEvent; sysex.sampleOffset = 17;
    sysex.data.type = DataEvent::kMidiSysEx; sysex.data.bytes = bytes.data(); sysex.data.size = static_cast<uint32>(bytes.size());
    plugin.incoming.events = {note, sysex, note}; plugin.incoming.events.back().noteOn.pitch = 61;
    bool checked = false;
    for (int block = 0; block < 5; ++block)
    {
        CHECK(plugin.process() == kResultOk); plugin.data.inputParameterChanges = nullptr; plugin.incoming.events.clear();
        const auto& events = plugin.outgoing.events;
        if (events.empty()) continue;
        CHECK(events.size() == 4);
        for (const auto& event : events) CHECK(event.sampleOffset == 17);
        CHECK(events[0].type == Event::kLegacyMIDICCOutEvent && events[0].midiCCOut.controlNumber == 7);
        CHECK(events[1].type == Event::kNoteOnEvent && events[1].noteOn.pitch == 60);
        CHECK(events[2].type == Event::kDataEvent && std::memcmp(events[2].data.bytes, bytes.data(), bytes.size()) == 0);
        CHECK(events[3].type == Event::kNoteOnEvent && events[3].noteOn.pitch == 61);
        checked = true;
    }
    CHECK(checked);
}
