#pragma once
#include "common/Native.h"
#include "common/Processing.h"
#include <memory>
#include <array>

namespace nytera
{
constexpr uint32_t audioSlotCount = 8;
constexpr uint32_t midiCapacity = 4096;
constexpr uint32_t midiByteCapacity = 1024 * 1024;

struct MidiDescriptor { int64_t sample; uint32_t offset, length; };
struct SharedMidi
{
    uint32_t count = 0, used = 0;
    std::array<MidiDescriptor, midiCapacity> events;
    std::array<std::byte, midiByteCapacity> bytes;
    bool push(int64_t sample, std::span<const std::byte>) noexcept;
    bool valid() const noexcept;
};
struct TransportMarker { uint32_t offset, reserved; TransportSnapshot snapshot; };
struct SharedTransport
{
    uint32_t count = 0, reserved = 0;
    std::array<TransportMarker, 4096> markers;
    bool valid(uint32_t quantum, int64_t start) const noexcept;
};
struct SharedParameters
{
    uint32_t count = 0, reserved = 0;
    std::array<ParameterEvent, 8192> events;
    bool push(ParameterEvent) noexcept;
    bool valid(uint32_t quantum) const noexcept;
};
static_assert(sizeof(TransportMarker) == 80);
struct alignas(64) SharedAudioHeader
{
    uint32_t magic, version, quantum, inputs, outputs, slots;
    volatile LONG generation, error, underruns;
    uint32_t parameterCount, reserved[6];
};
struct alignas(64) AudioSlotHeader
{
    volatile LONG state;
    uint32_t generation;
    uint64_t sequence;
    TransportSnapshot transport;
    uint32_t frames, error;
    std::byte reserved[32];
};
static_assert(sizeof(SharedAudioHeader) == 64);
static_assert(sizeof(AudioSlotHeader) == 128);
static_assert(offsetof(AudioSlotHeader, transport) == 16);
static_assert(sizeof(MidiDescriptor) == 16);

class SharedAudio
{
public:
    static Result<std::unique_ptr<SharedAudio>> create(const std::wstring&, const ProcessConfig&);
    static Result<std::unique_ptr<SharedAudio>> open(const std::wstring&, const ProcessConfig&);
    ~SharedAudio();
    SharedAudioHeader& header() const { return *static_cast<SharedAudioHeader*>(view); }
    AudioSlotHeader& slot(uint32_t index) const;
    float* input(uint32_t slot, uint32_t channel) const;
    float* output(uint32_t slot, uint32_t channel) const;
    SharedMidi& inputMidi(uint32_t slot) const;
    SharedMidi& outputMidi(uint32_t slot) const;
    SharedTransport& transportMarkers(uint32_t slot) const;
    SharedParameters& parameters(uint32_t slot) const;
    HANDLE readyEvent() const { return ready.get(); }
    HANDLE doneEvent() const { return done.get(); }
    const ProcessConfig config;
    const uint32_t quantum;
    const std::wstring name;
private:
    SharedAudio(std::wstring, ProcessConfig);
    static Result<std::unique_ptr<SharedAudio>> map(const std::wstring&, const ProcessConfig&, bool create);
    void* view = nullptr;
    Handle mapping, ready, done;
    size_t inputOffset, outputOffset, inputMidiOffset, outputMidiOffset, transportOffset, parameterOffset, slotBytes, totalBytes;
};

class AudioTransport
{
public:
    static Result<std::unique_ptr<AudioTransport>> create(const std::wstring&, const ProcessConfig&);
    ProcessResult process(AudioBlockView, EventBlockView, const TransportSnapshot&) noexcept;
    ProcessResult process(DoubleAudioBlockView, EventBlockView, const TransportSnapshot&) noexcept;
    void reset() noexcept;
    uint32_t latency() const noexcept { return memory->quantum * 2; }
    const std::wstring& name() const { return memory->name; }
    SharedAudio& shared() { return *memory; }
private:
    explicit AudioTransport(std::unique_ptr<SharedAudio>);
    bool submit() noexcept;
    int findOutput(uint64_t sequence, bool offline) noexcept;
    void reap(uint64_t before) noexcept;
    std::unique_ptr<SharedAudio> memory;
    std::vector<float> inputScratch, precisionInput, precisionOutput;
    std::unique_ptr<SharedMidi> midiScratch;
    std::unique_ptr<SharedTransport> transportScratch;
    std::unique_ptr<SharedParameters> parameterScratch;
    std::vector<float> parameterValues;
    std::vector<uint8_t> parameterSeen;
    std::vector<uint64_t> parameterSequences;
    TransportSnapshot inputTransport;
    int64_t clock = 0, epoch = 0, expectedProject = 0;
    uint64_t outputSequence = UINT64_MAX;
    uint32_t generation = 1, filled = 0;
    int outputSlot = -1;
    bool hadTransport = false, wasPlaying = false, pendingNotesOff = false;
};
}
