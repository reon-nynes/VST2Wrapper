#pragma once
#include "common/Manifest.h"
#include "common/Vst2.h"
#include "common/Processing.h"
#include "common/State.h"
#include <optional>
#include <windows.h>
#include <memory>
#include <functional>
#include <atomic>

namespace nytera
{
class Vst2Instance
{
public:
    static Result<std::unique_ptr<Vst2Instance>> open(const std::filesystem::path&);
    ~Vst2Instance();
    Result<Manifest> inspect();
    Status prepare(const ProcessConfig&);
    void suspend();
    Status applyParameter(const ParameterEvent&);
    uint64_t parameterSequence(uint32_t index) const noexcept;
    Result<std::string> parameterDisplay(uint32_t);
    Status refreshLatency();
    double tailSeconds();
    void publishTransport(const TransportSnapshot&) noexcept;
    Result<ProgramInfo> programInfo();
    Result<StateEnvelope> captureState();
    Status restoreState(const StateEnvelope&, uint64_t sequenceBarrier = 0);
    Status setProgram(uint32_t, uint64_t sequence = 0, bool mirror = false);
    Status openEditor(void* parent);
    Status process(AudioBlockView, EventBlockView, const TransportSnapshot&);
    void resetEvents();
    void close() noexcept;
    intptr_t dispatch(int32_t opcode, int32_t index = 0, intptr_t value = 0, void* pointer = nullptr, float real = 0);
    vst_effect_t* native() const { return effect; }
    std::function<void(const HostNotification&)> onNotification;
private:
    Vst2Instance() = default;
    static intptr_t VST_FUNCTION_INTERFACE host(vst_effect_t*, int32_t, int32_t, intptr_t, void*, float);
    intptr_t hostCall(int32_t, int32_t, intptr_t, void*, float);
    HMODULE module = nullptr;
    vst_effect_t* effect = nullptr;
    std::filesystem::path source;
    std::string sourceDirectory;
    ProcessConfig config;
    bool readTransport(TransportSnapshot&) const noexcept;
    std::atomic<double> callbackRate {44100};
    std::atomic<uint32_t> callbackBlock {512};
    std::atomic<uint32_t> transportSequence {0};
    std::array<std::atomic<uint64_t>, 9> transportWords {};
    bool opened = false, prepared = false;
    std::unique_ptr<Vst2Events> events;
    std::vector<vst_event_midi_t> midiEvents;
    std::vector<vst_event_midi_sysex_t> sysexEvents;
    std::vector<double> doubleInputs, doubleOutputs;
    std::vector<uint64_t> parameterSequences;
    std::unique_ptr<std::atomic<uint64_t>[]> callbackParameterSequences;
    std::vector<float> parameterValues;
    uint64_t programSequence = 0;
    std::optional<Manifest> binding;
    std::vector<float> dryDelay;
    uint32_t dryPosition = 0, dryLatency = 0;
    bool flushNotes = false;
    bool hasProcessed = false;
    bool invalidOutputEvents = false;
    uint32_t processingSamples = 0;
    static thread_local Vst2Instance* processing;
    static thread_local const TransportSnapshot* processTransport;
    static thread_local EventBlockView* processEvents;
    static thread_local Vst2Instance* constructing;
    static thread_local Vst2Instance* applyingParameter;
};
}
