#include "bridge/BridgeClient.h"
#include "bridge/AudioTransport.h"
#include <thread>
#include "helper/Vst2Instance.h"
#include "Test.h"
#include <algorithm>
#include <array>
#include <cmath>
using namespace nytera;
struct AudioSession
{
    BridgeClient client;
    Manifest manifest;
    uint32_t maximum;
    int64_t position = 0;
    std::array<float, 512> input {}, left {}, right {};
    explicit AudioSession(uint32_t maximum = 64, std::string variant = "NyteraFixture.dll", bool x86 = false, bool offline = true) : maximum(maximum)
    {
        auto root = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build";
        auto source = root / "x64/" NYTERA_TEST_CONFIG / variant;
        { auto instance = require(Vst2Instance::open(source)); manifest = require(instance->inspect()); }
        auto folder = root / (x86 ? "x86" : "x64") / NYTERA_TEST_CONFIG;
        source = folder / variant;
        manifest.architecture = x86 ? Architecture::x86 : Architecture::x64;
        manifest.sourceHash = sha256(readFile(source));
        manifest = assignBindingIds(manifest);
        require(client.start(manifest, folder / "NyteraBridge.exe", source));
        require(client.prepare({48000, maximum, static_cast<uint32_t>(manifest.inputs.size()), static_cast<uint32_t>(manifest.outputs.size()), offline}));
    }
    ProcessResult block(uint32_t count, std::span<const MidiEvent> midi = {}, void* context = nullptr,
                        void (*emit)(void*, int64_t, std::span<const std::byte>) = nullptr, int64_t project = -1, double bpm = 123, bool playing = true, bool bypass = false)
    {
        std::array<const float*, 2> inputs {input.data(), input.data()};
        std::array<float*, 2> outputs {left.data(), right.data()};
        TransportSnapshot transport;
        transport.flags = (playing ? 2 : 0) | 512 | 1024 | 8192;
        transport.projectSamples = project < 0 ? position : project;
        transport.tempo = bpm;
        if (bypass) transport.processFlags |= 2;
        transport.numerator = 7; transport.denominator = 8;
        auto result = client.process(AudioBlockView{std::span(inputs).first(manifest.inputs.size()), outputs, count}, {midi, context, emit}, transport);
        position += count;
        return result;
    }
};
static std::vector<float> impulse(bool variable, bool zeroBlocks, bool x86 = false)
{
    AudioSession session(variable ? 512 : 64, "NyteraDelayFixture.dll", x86);
    CHECK(session.client.latencySamples() == 16u);
    std::vector<float> result;
    result.reserve(4096);
    const uint32_t sizes[] {1, 63, 64, 127, 256, 511};
    size_t index = 0;
    while (result.size() < 2048)
    {
        session.input.fill(0);
        if (session.position == 0) session.input[0] = 1;
        auto count = variable ? sizes[index++ % std::size(sizes)] : 64;
        if (zeroBlocks) CHECK(session.block(0) != ProcessResult::failed);
        CHECK(session.block(count) != ProcessResult::failed);
        result.insert(result.end(), session.left.begin(), session.left.begin() + count);
    }
    return result;
}
TEST("Audio.ImpulseLatency")
{
    for (bool x86 : {false, true})
    {
        auto samples = impulse(false, false, x86);
        for (size_t i = 0; i < samples.size(); ++i) CHECK(samples[i] == (i == 16 ? 0.5f : 0.0f));
    }
}
TEST("Audio.VariableBlocks")
{
    auto samples = impulse(true, false);
    for (size_t i = 0; i < samples.size(); ++i) CHECK(samples[i] == (i == 16 ? 0.5f : 0.0f));
}
TEST("Audio.ZeroBlock") { CHECK(impulse(true, true) == impulse(true, false)); }
TEST("Audio.ZeroLatency")
{
    for (bool x86 : {false, true})
        for (bool offline : {false, true})
            for (uint32_t maximum : {63u, 64u, 512u})
            {
                AudioSession session(maximum, "NyteraFixture.dll", x86, offline);
                CHECK(session.client.latencySamples() == 0);
                for (uint32_t frames : {1u, maximum, 17u, 0u, maximum - 1})
                {
                    session.input.fill(0.8f);
                    session.left.fill(-1);
                    CHECK(session.block(frames) != ProcessResult::failed);
                    for (uint32_t i = 0; i < frames; ++i) CHECK(session.left[i] == 0.4f);
                    if (frames < session.left.size()) CHECK(session.left[frames] == -1);
                }
            }
}
TEST("Audio.ZeroBlockEvents")
{
    for (bool x86 : {false, true})
    {
        AudioSession session(64, "NyteraInstrumentFixture.dll", x86);
        std::array<std::byte, 3> on {std::byte{0x90}, std::byte{60}, std::byte{100}};
        MidiEvent note {0, on};
        session.client.enqueueParameter({0, 0, 0.75f, 1});
        CHECK(session.block(0, std::span(&note, 1)) != ProcessResult::failed);
        CHECK(session.block(0) != ProcessResult::failed);
        std::vector<int64_t> emitted;
        auto emit = [](void* context, int64_t sample, std::span<const std::byte>)
        { static_cast<std::vector<int64_t>*>(context)->push_back(sample); };
        CHECK(session.block(1, {}, &emitted, emit) == ProcessResult::rendered);
        CHECK(session.left[0] == 0.75f);
        CHECK(emitted == std::vector<int64_t>{0});
        CHECK(session.block(1, {}, &emitted, emit) == ProcessResult::rendered);
        CHECK(emitted.size() == 1);
    }
}
TEST("Audio.InPlace")
{
    for (bool x86 : {false, true})
    {
        AudioSession session(63, "NyteraFixture.dll", x86, false);
        std::array<float, 63> left, right;
        left.fill(0.8f); right.fill(0.2f);
        std::array<const float*, 2> inputs {left.data(), right.data()};
        std::array<float*, 2> outputs {left.data(), right.data()};
        CHECK(session.client.process(AudioBlockView{inputs, outputs, 17}, {}, {}) == ProcessResult::rendered);
        for (size_t i = 0; i < left.size(); ++i)
        {
            CHECK(left[i] == (i < 17 ? 0.4f : 0.8f));
            CHECK(right[i] == (i < 17 ? 0.1f : 0.2f));
        }
    }
}
TEST("Audio.InvalidCompletion")
{
    for (int fault = 0; fault < 4; ++fault)
    {
        auto name = L"Local\\Nytera-invalid-completion-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(fault);
        auto audio = require(AudioTransport::create(name, {48000, 64, 2, 2, false}));
        std::jthread worker([&]
        {
            if (WaitForSingleObject(audio->shared().readyEvent(), 2000) != WAIT_OBJECT_0) return;
            auto& slot = audio->shared().slot(0);
            if (fault == 0) ++slot.sequence;
            if (fault == 1) ++slot.generation;
            if (fault == 2) ++slot.frames;
            if (fault == 3)
            {
                std::array<std::byte, 3> note {std::byte{0x90}, std::byte{60}, std::byte{100}};
                audio->shared().outputMidi(0).push(64, note);
            }
            InterlockedExchange(&slot.state, 3);
            SetEvent(audio->shared().doneEvent());
        });
        std::array<float, 64> input {}, left, right;
        left.fill(1); right.fill(1);
        std::array<const float*, 2> inputs {input.data(), input.data()};
        std::array<float*, 2> outputs {left.data(), right.data()};
        CHECK(audio->process(AudioBlockView{inputs, outputs, 64}, {}, {}) == ProcessResult::failed);
        CHECK(std::all_of(left.begin(), left.end(), [](float value) { return value == 0; }));
        CHECK(InterlockedCompareExchange(&audio->shared().slot(0).state, 0, 0) == 3);
    }
}
TEST("Audio.SynchronousTimeout")
{
    auto audio = require(AudioTransport::create(L"Local\\Nytera-timeout-" + std::to_wstring(GetCurrentProcessId()), {48000, 64, 2, 2, false}));
    std::array<float, 64> input {}, left, right;
    left.fill(1); right.fill(1);
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {left.data(), right.data()};
    auto before = std::chrono::steady_clock::now();
    CHECK(audio->process(AudioBlockView{inputs, outputs, 64}, {}, {}) == ProcessResult::failed);
    auto elapsed = std::chrono::steady_clock::now() - before;
    CHECK(elapsed >= std::chrono::milliseconds(200) && elapsed < std::chrono::seconds(2));
    CHECK(std::all_of(left.begin(), left.end(), [](float value) { return value == 0; }));
    CHECK(InterlockedCompareExchange(&audio->shared().header().error, 0, 0) != 0);
    CHECK(InterlockedCompareExchange(&audio->shared().slot(0).state, 0, 0) == 1);
    CHECK(audio->process(AudioBlockView{inputs, outputs, 64}, {}, {}) == ProcessResult::failed);
}
TEST("Audio.SynchronousCancel")
{
    auto audio = require(AudioTransport::create(L"Local\\Nytera-cancel-" + std::to_wstring(GetCurrentProcessId()), {48000, 64, 2, 2, true}));
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    audio->setStopEvent(stop.get());
    ProcessResult result = ProcessResult::rendered;
    std::array<float, 64> input {}, left, right;
    left.fill(1); right.fill(1);
    std::array<const float*, 2> inputs {input.data(), input.data()};
    std::array<float*, 2> outputs {left.data(), right.data()};
    std::jthread callback([&] { result = audio->process(AudioBlockView{inputs, outputs, 64}, {}, {}); });
    CHECK(WaitForSingleObject(audio->shared().readyEvent(), 2000) == WAIT_OBJECT_0);
    auto before = std::chrono::steady_clock::now();
    SetEvent(stop.get());
    callback.join();
    CHECK(result == ProcessResult::failed);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
    CHECK(std::all_of(left.begin(), left.end(), [](float value) { return value == 0; }));
}
TEST("Audio.ZeroBlockCleanup")
{
    AudioSession session(64, "NyteraInstrumentFixture.dll");
    std::vector<std::pair<int64_t, Bytes>> outgoing;
    auto emit = [](void* context, int64_t sample, std::span<const std::byte> message)
    { static_cast<decltype(outgoing)*>(context)->push_back({sample, Bytes(message.begin(), message.end())}); };
    std::array<std::byte, 3> on {std::byte{0x90}, std::byte{60}, std::byte{100}};
    MidiEvent note {0, on};
    for (int block = 0; block < 4; ++block)
        CHECK(session.block(64, block == 0 ? std::span(&note, 1) : std::span<MidiEvent>{}, &outgoing, emit) != ProcessResult::failed);
    CHECK(outgoing.size() == 1 && outgoing[0].second == Bytes(on.begin(), on.end()));
    outgoing.clear();
    CHECK(session.block(0, {}, &outgoing, emit, 10000, 123, false) != ProcessResult::failed);
    CHECK(outgoing.empty());
    CHECK(session.block(64, {}, &outgoing, emit, 10000, 123, false) != ProcessResult::failed);
    CHECK(outgoing.size() == 48);
    for (uint8_t channel = 0; channel < 16; ++channel)
        CHECK(outgoing[channel].second == Bytes({static_cast<std::byte>(0xb0 | channel), std::byte{123}, std::byte{0}}));
    for (uint8_t i = 0; i < 32; ++i)
        CHECK(outgoing[16 + i].second == Bytes({static_cast<std::byte>(0xb0 | (i / 2)), static_cast<std::byte>(i % 2 == 0 ? 123 : 120), std::byte{0}}));
    outgoing.clear();
    CHECK(session.block(64, {}, &outgoing, emit, 10000, 123, false) != ProcessResult::failed);
    CHECK(outgoing.empty());
}
TEST("Audio.SeekFlush")
{
    AudioSession session;
    session.input[0] = 1;
    CHECK(session.block(64) != ProcessResult::failed);
    session.input.fill(0);
    CHECK(session.block(64) != ProcessResult::failed);
    for (int i = 0; i < 8; ++i)
    {
        CHECK(session.block(64, {}, nullptr, nullptr, 10000 + i * 64) != ProcessResult::failed);
        CHECK(std::all_of(session.left.begin(), session.left.begin() + 64, [](float value) { return value == 0; }));
    }
}
static void midiAlignment(bool x86)
{
    AudioSession session(64, "NyteraInstrumentFixture.dll", x86);
    std::array<std::byte, 3> on {std::byte{0x90}, std::byte{60}, std::byte{100}};
    std::array<std::byte, 3> off {std::byte{0x80}, std::byte{60}, std::byte{0}};
    std::array<MidiEvent, 2> events {{{10, on}, {40, off}}};
    std::vector<std::pair<int64_t, Bytes>> outgoing;
    auto emit = [](void* context, int64_t sample, std::span<const std::byte> data)
    {
        static_cast<decltype(outgoing)*>(context)->push_back({sample, Bytes(data.begin(), data.end())});
    };
    std::vector<float> samples;
    for (int i = 0; i < 6; ++i)
    {
        CHECK(session.block(64, i == 0 ? std::span<const MidiEvent>(events) : std::span<const MidiEvent>{}, &outgoing, emit) != ProcessResult::failed);
        samples.insert(samples.end(), session.left.begin(), session.left.begin() + 64);
    }
    for (size_t i = 0; i < samples.size(); ++i) CHECK(samples[i] == (i >= 10 && i < 40 ? 0.5f : 0.0f));
    CHECK(outgoing.size() == 2);
    CHECK(outgoing[0].first == 10 && outgoing[1].first == 40);
    CHECK(outgoing[0].second == Bytes(on.begin(), on.end()));
}
TEST("Audio.MidiAlignment") { midiAlignment(false); midiAlignment(true); }
TEST("Audio.Sysex")
{
    for (bool x86 : {false, true})
    {
        AudioSession session(64, "NyteraInstrumentFixture.dll", x86);
        Bytes message(65536, std::byte{0x42});
        message.front() = std::byte{0xf0}; message.back() = std::byte{0xf7};
        MidiEvent event {17, message};
        std::vector<std::pair<int64_t, Bytes>> outgoing;
        auto emit = [](void* context, int64_t sample, std::span<const std::byte> data)
        { static_cast<decltype(outgoing)*>(context)->push_back({sample, Bytes(data.begin(), data.end())}); };
        for (int i = 0; i < 5; ++i)
            CHECK(session.block(64, i == 0 ? std::span(&event, 1) : std::span<MidiEvent>{}, &outgoing, emit) != ProcessResult::failed);
        CHECK(outgoing.size() == 1);
        CHECK(outgoing[0].first == 17 && outgoing[0].second == message);
    }
}
TEST("Audio.DoubleInput")
{
    AudioSession session;
    std::array<double, 64> input {}, left {}, right {};
    std::array<const double*, 2> inputs {input.data(), input.data()};
    std::array<double*, 2> outputs {left.data(), right.data()};
    for (int i = 0; i < 5; ++i)
    {
        input.fill(0); if (i == 0) input[0] = 2;
        TransportSnapshot snapshot; snapshot.projectSamples = i * 64; snapshot.flags = 2;
        CHECK(session.client.process(DoubleAudioBlockView{inputs, outputs, 64}, {}, snapshot) != ProcessResult::failed);
        CHECK(left[0] == (i == 0 ? 1.0 : 0.0));
    }
}
TEST("Audio.Offline")
{
    AudioSession session(64, "NyteraTimeFixture.dll");
    for (int block = 0; block < 8; ++block)
    {
        CHECK(session.block(64) != ProcessResult::failed);
        for (int sample = 0; sample < 64; ++sample)
            CHECK(session.left[static_cast<size_t>(sample)] == static_cast<float>(block * 64 + sample));
    }
}
TEST("Audio.CallbackAllocations")
{
    AudioSession session;
    allocationCount = 0;
    countAllocations = true;
    for (int i = 0; i < 16; ++i)
    {
        session.client.enqueueParameter({0, 0, i % 2 ? 0.25f : 0.75f, 1});
        session.block(64);
    }
    countAllocations = false;
    CHECK(allocationCount == 0);
}
TEST("Audio.StoppedPosition")
{
    AudioSession session(64, "NyteraTimeFixture.dll");
    std::vector<float> samples;
    for (int i = 0; i < 8; ++i)
    {
        CHECK(session.block(48, {}, nullptr, nullptr, 1000, 123, false) != ProcessResult::failed);
        samples.insert(samples.end(), session.left.begin(), session.left.begin() + 48);
    }
    CHECK(samples[0] == 1000);
    CHECK(samples[48] == 1000);
    CHECK(samples[47] == 1047);
}
TEST("Audio.TempoBoundary")
{
    AudioSession session(64, "NyteraTimeFixture.dll");
    std::vector<float> samples;
    for (int i = 0; i < 12; ++i)
    {
        CHECK(session.block(32, {}, nullptr, nullptr, -1, i == 0 ? 123 : 124) != ProcessResult::failed);
        samples.insert(samples.end(), session.left.begin(), session.left.begin() + 32);
    }
    CHECK(samples[0] == 0);
    CHECK(samples[31] == 31);
    CHECK(samples[32] == -999);
}
TEST("Audio.BypassLatency")
{
    AudioSession session(64, "NyteraDelayFixture.dll");
    std::vector<float> samples;
    for (int i = 0; i < 8; ++i)
    {
        session.input.fill(0);
        if (i == 0) session.input[0] = 1;
        CHECK(session.block(64, {}, nullptr, nullptr, -1, 123, true, true) != ProcessResult::failed);
        samples.insert(samples.end(), session.left.begin(), session.left.begin() + 64);
    }
    for (size_t i = 0; i < samples.size(); ++i) CHECK(samples[i] == (i == session.client.latencySamples() ? 1.0f : 0.0f));
}
TEST("Audio.DoubleInvalid")
{
    AudioSession session;
    std::array<double, 64> output;
    output.fill(1);
    std::array<double*, 2> outputs {output.data(), output.data()};
    CHECK(session.client.process(DoubleAudioBlockView{{}, outputs, 64}, {}, {}) == ProcessResult::failed);
    CHECK(std::all_of(output.begin(), output.end(), [](double value) { return value == 0; }));
}
TEST("Audio.SuspendResume")
{
    AudioSession session;
    CHECK(session.block(64) != ProcessResult::failed);
    require(session.client.releaseAudio());
    CHECK(session.block(64) == ProcessResult::failed);
    require(session.client.prepare({48000, 64, 2, 2, true}));
    session.position = 0;
    session.input.fill(1);
    for (int i = 0; i < 4; ++i) CHECK(session.block(64) != ProcessResult::failed);
    CHECK(session.left[0] == 0.5f);
}
TEST("Audio.SeekNotes")
{
    AudioSession session(64, "NyteraInstrumentFixture.dll");
    std::array<std::byte, 3> on {std::byte{0x90}, std::byte{60}, std::byte{100}};
    std::array<std::byte, 3> modulation {std::byte{0xb0}, std::byte{1}, std::byte{50}};
    MidiEvent note {0, on};
    for (int i = 0; i < 4; ++i) CHECK(session.block(64, i == 0 ? std::span(&note, 1) : std::span<MidiEvent>{}) != ProcessResult::failed);
    CHECK(session.left[0] == 0.5f);
    MidiEvent control {session.position, modulation};
    for (int i = 0; i < 6; ++i)
    {
        CHECK(session.block(64, i == 0 ? std::span(&control, 1) : std::span<MidiEvent>{}, nullptr, nullptr, 10000 + i * 64) != ProcessResult::failed);
        CHECK(std::all_of(session.left.begin(), session.left.begin() + 64, [](float value) { return value == 0; }));
    }
}
