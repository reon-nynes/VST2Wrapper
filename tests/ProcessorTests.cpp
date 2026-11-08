#include "helper/Vst2Instance.h"
#include "Vst3Host.h"
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
using namespace nytera;
using namespace Steinberg;
using namespace Steinberg::Vst;
struct RunningProcessor
{
    FactoryModule module;
    IComponent* component = nullptr;
    IAudioProcessor* processor = nullptr;
    explicit RunningProcessor(const std::filesystem::path& path) : module(path)
    {
        auto info = module.component();
        CHECK(module.factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
        CHECK(component->initialize(nullptr) == kResultOk);
        CHECK(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&processor)) == kResultOk);
    }
    ~RunningProcessor()
    {
        if (processor) processor->setProcessing(false);
        if (component) { component->setActive(false); component->terminate(); }
        if (processor) processor->release();
        if (component) component->release();
    }
};
static std::filesystem::path packageProcessor(const std::string& name)
{
    auto folder = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/processor-tests" / (name + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    Manifest manifest;
    { auto instance = require(Vst2Instance::open(NYTERA_FIXTURE)); manifest = require(instance->inspect()); }
    auto helper = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build/x64/Debug/NyteraBridge.exe";
    auto output = folder / "Fixture.vst3";
    require(writePackage({manifest, NYTERA_EFFECT_TEMPLATE, helper, NYTERA_FIXTURE, {}}, output));
    return output;
}
template<class Sample> static void checkProcessing()
{
    constexpr int precision = sizeof(Sample) == 8 ? kSample64 : kSample32;
    RunningProcessor loaded(packageProcessor(sizeof(Sample) == 8 ? "double" : "float"));
    CHECK(loaded.processor->canProcessSampleSize(precision) == kResultOk);
    ProcessSetup setup {kOffline, precision, 64, 48000};
    CHECK(loaded.processor->setupProcessing(setup) == kResultOk);
    CHECK(loaded.component->activateBus(kAudio, kInput, 0, true) == kResultOk);
    CHECK(loaded.component->activateBus(kAudio, kOutput, 0, true) == kResultOk);
    CHECK(loaded.component->setActive(true) == kResultOk);
    CHECK(loaded.processor->setProcessing(true) == kResultOk);
    CHECK(loaded.processor->getLatencySamples() == 128);
    std::array<Sample, 64> input {}, left {}, right {};
    std::array<Sample*, 2> inputs {input.data(), input.data()}, outputs {left.data(), right.data()};
    AudioBusBuffers in {}, out {};
    in.numChannels = out.numChannels = 2;
    if constexpr (sizeof(Sample) == 8) { in.channelBuffers64 = inputs.data(); out.channelBuffers64 = outputs.data(); }
    else { in.channelBuffers32 = inputs.data(); out.channelBuffers32 = outputs.data(); }
    ProcessContext context {};
    context.sampleRate = 48000;
    context.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;
    context.tempo = 120; context.timeSigNumerator = 4; context.timeSigDenominator = 4;
    ProcessData data;
    data.processMode = kOffline; data.symbolicSampleSize = precision; data.numSamples = 64;
    data.numInputs = data.numOutputs = 1; data.inputs = &in; data.outputs = &out; data.processContext = &context;
    for (int block = 0; block < 6; ++block)
    {
        input.fill(0); left.fill(-1); right.fill(-1);
        if (block == 0) input[0] = 1;
        context.projectTimeSamples = block * 64;
        CHECK(loaded.processor->process(data) == kResultOk);
        for (int i = 0; i < 64; ++i) CHECK(left[static_cast<size_t>(i)] == (block == 2 && i == 0 ? Sample{0.5} : Sample{0}));
    }
}
TEST("Processor.FloatEntry") { checkProcessing<float>(); }
TEST("Processor.DoubleEntry") { checkProcessing<double>(); }
struct TwoPoints : IParamValueQueue
{
    ParamID id = 0;
    tresult PLUGIN_API queryInterface(const TUID, void** object) override { *object = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return 2; }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override
    {
        if (index < 0 || index > 1) return kResultFalse;
        offset = index == 0 ? 0 : 32;
        value = index == 0 ? 0.25 : 0.75;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kResultFalse; }
};
struct OneParameter : IParameterChanges
{
    TwoPoints points;
    tresult PLUGIN_API queryInterface(const TUID, void** object) override { *object = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override { return index == 0 ? &points : nullptr; }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override { return nullptr; }
};
TEST("Parameter.Resolution")
{
    auto path = packageProcessor("resolution");
    auto manifest = require(readPackageManifest(path));
    RunningProcessor loaded(path);
    ProcessSetup setup {kOffline, kSample32, 64, 48000};
    CHECK(loaded.processor->setupProcessing(setup) == kResultOk);
    loaded.component->activateBus(kAudio, kInput, 0, true);
    loaded.component->activateBus(kAudio, kOutput, 0, true);
    CHECK(loaded.component->setActive(true) == kResultOk);
    loaded.processor->setProcessing(true);
    std::array<float, 64> input, left, right;
    input.fill(1);
    std::array<float*, 2> inputs {input.data(), input.data()}, outputs {left.data(), right.data()};
    AudioBusBuffers in {}, out {};
    in.numChannels = out.numChannels = 2;
    in.channelBuffers32 = inputs.data(); out.channelBuffers32 = outputs.data();
    OneParameter changes;
    changes.points.id = manifest.parameters[0].vst3Id;
    ProcessContext context {}; context.sampleRate = 48000; context.state = ProcessContext::kPlaying;
    ProcessData data;
    data.processMode = kOffline; data.symbolicSampleSize = kSample32; data.numSamples = 64;
    data.numInputs = data.numOutputs = 1; data.inputs = &in; data.outputs = &out; data.processContext = &context;
    for (int block = 0; block < 5; ++block)
    {
        context.projectTimeSamples = block * 64;
        data.inputParameterChanges = block == 0 ? &changes : nullptr;
        CHECK(loaded.processor->process(data) == kResultOk);
        for (float sample : left) CHECK(sample == (block < 2 ? 0.0f : 0.75f));
    }
}
