#pragma once
#include "Types.h"
namespace nytera
{
struct ProcessConfig
{
    double sampleRate = 44100;
    uint32_t maximumBlock = 512;
    uint32_t inputs = 2, outputs = 2;
    bool offline = false;
    uint32_t parameterCount = 0;
    uint32_t programCount = 1;
};
struct HostNotification
{
    uint32_t opcode = 0;
    int32_t index = 0;
    int64_t value = 0;
    float normalized = 0;
};
struct TransportSnapshot
{
    uint32_t flags = 0;
    uint32_t processFlags = 0;
    int64_t projectSamples = 0;
    int64_t streamSamples = 0;
    double ppq = 0, tempo = 120, barStart = 0, cycleStart = 0, cycleEnd = 0;
    int32_t numerator = 4, denominator = 4;
};
static_assert(sizeof(TransportSnapshot) == 72);
struct AudioBlockView
{
    std::span<const float* const> inputs;
    std::span<float* const> outputs;
    uint32_t samples = 0;
};
struct DoubleAudioBlockView
{
    std::span<const double* const> inputs;
    std::span<double* const> outputs;
    uint32_t samples = 0;
};
struct MidiEvent
{
    int64_t sample = 0;
    std::span<const std::byte> data;
};
struct ParameterEvent
{
    uint32_t index = 0, offset = 0;
    float normalized = 0;
    uint32_t origin = 1; // 1/2 are host/source values, 3/4 are host/source programs.
    uint64_t sequence = 0;
};
struct ParameterDisplay { float normalized = 0; std::string text; };
static_assert(sizeof(ParameterEvent) == 24);
struct EventBlockView
{
    std::span<const MidiEvent> input;
    void* context = nullptr;
    void (*emit)(void*, int64_t sample, std::span<const std::byte>) = nullptr;
    std::span<const ParameterEvent> parameters;
};
enum class ProcessResult { rendered, priming, underrun, failed };
}
