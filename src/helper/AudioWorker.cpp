#include "AudioWorker.h"
#include <algorithm>
#include <avrt.h>
#include <objbase.h>
namespace nytera
{
AudioWorker::AudioWorker(std::unique_ptr<SharedAudio> memory, Vst2Instance& source, std::mutex& mutex)
    : memory(std::move(memory)), source(source), sourceMutex(mutex)
{
    midiViews.reserve(midiCapacity);
    thread = std::jthread([this](std::stop_token token) { run(token); });
}
AudioWorker::~AudioWorker()
{
    thread.request_stop();
    SetEvent(memory->readyEvent());
    if (thread.joinable()) thread.join();
}
void AudioWorker::run(std::stop_token token)
{
    auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE scheduling = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!scheduling) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (!token.stop_requested())
    {
        int selected = -1;
        uint64_t sequence = UINT64_MAX;
        auto generation = static_cast<uint32_t>(InterlockedCompareExchange(&memory->header().generation, 0, 0));
        for (uint32_t i = 0; i < audioSlotCount; ++i)
        {
            auto& slot = memory->slot(i);
            if (InterlockedCompareExchange(&slot.state, 0, 0) == 1 && slot.generation == generation && slot.sequence < sequence)
            { selected = static_cast<int>(i); sequence = slot.sequence; }
        }
        if (selected < 0) { WaitForSingleObject(memory->readyEvent(), 100); continue; }
        auto index = static_cast<uint32_t>(selected);
        auto& slot = memory->slot(index);
        if (InterlockedCompareExchange(&slot.state, 2, 1) != 1) continue;
        if (slot.generation != static_cast<uint32_t>(InterlockedCompareExchange(&memory->header().generation, 0, 0)))
        { InterlockedExchange(&slot.state, 3); SetEvent(memory->doneEvent()); continue; }
        auto& incoming = memory->inputMidi(index);
        auto& outgoing = memory->outputMidi(index);
        auto& markers = memory->transportMarkers(index);
        auto& parameters = memory->parameters(index);
        if (slot.frames != memory->quantum || !incoming.valid() || !markers.valid(memory->quantum, slot.transport.streamSamples) || !parameters.valid(memory->quantum))
        { InterlockedExchange(&memory->header().error, 1); InterlockedExchange(&slot.state, 3); SetEvent(memory->doneEvent()); continue; }
        std::array<const float*, 256> inputs;
        std::array<float*, 256> outputs;
        for (uint32_t channel = 0; channel < memory->config.inputs; ++channel) inputs[channel] = memory->input(index, channel);
        for (uint32_t channel = 0; channel < memory->config.outputs; ++channel) outputs[channel] = memory->output(index, channel);
        auto emit = [](void* context, int64_t sample, std::span<const std::byte> data)
        {
            auto& midi = *static_cast<SharedMidi*>(context);
            if (!midi.push(sample, data)) midi.count = midiCapacity + 1;
        };
        {
            std::lock_guard lock(sourceMutex);
            if (slot.generation != static_cast<uint32_t>(InterlockedCompareExchange(&memory->header().generation, 0, 0)))
            {
                InterlockedExchange(&slot.state, 3);
                SetEvent(memory->doneEvent());
                continue;
            }
            if (lastGeneration != 0 && lastGeneration != slot.generation) source.resetEvents();
            bool changed = lastGeneration != slot.generation;
            lastGeneration = slot.generation;
            uint32_t start = 0, markerIndex = 0, parameterIndex = 0;
            auto snapshot = slot.transport;
            if (changed) snapshot.flags |= 1;
            while (start < memory->quantum)
            {
                if (markerIndex < markers.count && markers.markers[markerIndex].offset == start)
                    snapshot = markers.markers[markerIndex++].snapshot;
                source.publishTransport(snapshot);
                while (parameterIndex < parameters.count && parameters.events[parameterIndex].offset == start)
                {
                    const auto& event = parameters.events[parameterIndex++];
                    auto applied = source.applyParameter(event);
                    if (std::holds_alternative<Failure>(applied)) { slot.error = 1; InterlockedExchange(&memory->header().error, 1); }
                    else memory->acknowledge(event);
                }
                if (slot.error) break;
                uint32_t end = memory->quantum;
                if (markerIndex < markers.count) end = markers.markers[markerIndex].offset;
                if (parameterIndex < parameters.count) end = std::min(end, parameters.events[parameterIndex].offset);
                midiViews.clear();
                for (uint32_t i = 0; i < incoming.count; ++i)
                {
                    const auto& event = incoming.events[i];
                    if (event.sample >= snapshot.streamSamples && event.sample < snapshot.streamSamples + end - start)
                        midiViews.push_back({event.sample, std::span(incoming.bytes).subspan(event.offset, event.length)});
                }
                for (uint32_t channel = 0; channel < memory->config.inputs; ++channel) inputs[channel] = memory->input(index, channel) + start;
                for (uint32_t channel = 0; channel < memory->config.outputs; ++channel) outputs[channel] = memory->output(index, channel) + start;
                auto result = source.process({std::span(inputs).first(memory->config.inputs), std::span(outputs).first(memory->config.outputs), end - start},
                    {midiViews, &outgoing, emit}, snapshot);
                if (std::holds_alternative<Failure>(result) || !outgoing.valid())
                { slot.error = 1; InterlockedExchange(&memory->header().error, 1); break; }
                auto count = end - start;
                snapshot.streamSamples += count;
                if (snapshot.flags & 2)
                {
                    snapshot.projectSamples += count;
                    if (snapshot.flags & 512) snapshot.ppq += count * snapshot.tempo / (60.0 * memory->config.sampleRate);
                }
                snapshot.flags &= ~1u;
                start = end;
            }
        }
        InterlockedExchange(&slot.state, 3);
        SetEvent(memory->doneEvent());
    }
    if (scheduling) AvRevertMmThreadCharacteristics(scheduling);
    if (SUCCEEDED(com)) CoUninitialize();
}
}
