#include "bridge/BridgeClient.h"
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
    explicit AudioSession(uint32_t maximum = 64, std::string variant = "NyteraFixture.dll", bool x86 = false) : maximum(maximum)
    {
        auto root = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build";
        auto source = root / "x64/Debug" / variant;
        { auto instance = require(Vst2Instance::open(source)); manifest = require(instance->inspect()); }
        auto folder = root / (x86 ? "x86" : "x64") / "Debug";
        source = folder / variant;
        manifest.architecture = x86 ? Architecture::x86 : Architecture::x64;
        manifest.sourceHash = sha256(readFile(source));
        manifest = assignBindingIds(manifest);
        require(client.start(manifest, folder / "NyteraBridge.exe", source));
        require(client.prepare({48000, maximum, static_cast<uint32_t>(manifest.inputs.size()), static_cast<uint32_t>(manifest.outputs.size()), true}));
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
    CHECK(session.client.latencySamples() == (variable ? 1040u : 144u));
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
        for (size_t i = 0; i < samples.size(); ++i) CHECK(samples[i] == (i == 144 ? 0.5f : 0.0f));
    }
}
TEST("Audio.VariableBlocks")
{
    auto samples = impulse(true, false);
    for (size_t i = 0; i < samples.size(); ++i) CHECK(samples[i] == (i == 1040 ? 0.5f : 0.0f));
}
TEST("Audio.ZeroBlock") { CHECK(impulse(true, true) == impulse(true, false)); }
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
    for (size_t i = 0; i < samples.size(); ++i) CHECK(samples[i] == (i >= 138 && i < 168 ? 0.5f : 0.0f));
    CHECK(outgoing.size() == 2);
    CHECK(outgoing[0].first == 138 && outgoing[1].first == 168);
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
        CHECK(outgoing[0].first == 145 && outgoing[0].second == message);
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
        CHECK(left[0] == (i == 2 ? 1.0 : 0.0));
    }
}
TEST("Audio.Offline")
{
    AudioSession session(64, "NyteraTimeFixture.dll");
    for (int block = 0; block < 8; ++block)
    {
        CHECK(session.block(64) != ProcessResult::failed);
        for (int sample = 0; sample < 64; ++sample)
            CHECK(session.left[static_cast<size_t>(sample)] == (block < 2 ? 0.0f : static_cast<float>((block - 2) * 64 + sample)));
    }
}
TEST("Audio.CallbackAllocations")
{
    AudioSession session;
    allocationCount = 0;
    countAllocations = true;
    for (int i = 0; i < 16; ++i) session.block(64);
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
    CHECK(samples[128] == 1000);
    CHECK(samples[192] == 1000);
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
    CHECK(samples[128] == 0);
    CHECK(samples[159] == 31);
    CHECK(samples[160] == -999);
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
