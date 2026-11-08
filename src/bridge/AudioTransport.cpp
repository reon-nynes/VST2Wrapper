#include "AudioTransport.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <cmath>
#include <chrono>

namespace nytera
{
bool SharedMidi::push(int64_t sample, std::span<const std::byte> message) noexcept
{
    if (count >= midiCapacity || message.empty() || message.size() > midiByteCapacity - used) return false;
    uint32_t position = count;
    while (position > 0 && events[position - 1].sample > sample) { events[position] = events[position - 1]; --position; }
    events[position] = {sample, used, static_cast<uint32_t>(message.size())};
    std::memcpy(bytes.data() + used, message.data(), message.size());
    used += static_cast<uint32_t>(message.size());
    ++count;
    return true;
}
bool SharedMidi::valid() const noexcept
{
    if (count > midiCapacity || used > midiByteCapacity) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].offset > used || events[i].length > used - events[i].offset || events[i].length == 0
            || (i > 0 && events[i - 1].sample > events[i].sample)) return false;
    return true;
}
bool SharedTransport::valid(uint32_t quantum, int64_t start) const noexcept
{
    if (count > markers.size()) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (markers[i].offset == 0 || markers[i].offset >= quantum || markers[i].snapshot.streamSamples != start + markers[i].offset
            || (i > 0 && markers[i - 1].offset >= markers[i].offset)) return false;
    return true;
}
static size_t alignUp(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
bool SharedParameters::push(ParameterEvent event) noexcept
{
    if (count >= events.size()) return false;
    uint32_t index = count;
    while (index > 0 && (events[index - 1].offset > event.offset
        || (events[index - 1].offset == event.offset && events[index - 1].sequence > event.sequence)))
    { events[index] = events[index - 1]; --index; }
    events[index] = event;
    ++count;
    return true;
}
bool SharedParameters::valid(uint32_t quantum) const noexcept
{
    if (count > events.size()) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].offset >= quantum || !std::isfinite(events[i].normalized) || events[i].normalized < 0 || events[i].normalized > 1
            || events[i].origin < 1 || events[i].origin > 4 || (i > 0 && events[i - 1].offset > events[i].offset)) return false;
    return true;
}
SharedAudio::SharedAudio(std::wstring name, ProcessConfig config)
    : config(config), quantum(std::bit_ceil(config.maximumBlock)), name(std::move(name))
{
    if (!std::isfinite(config.sampleRate) || config.sampleRate < 1000 || config.sampleRate > 768000
        || config.maximumBlock == 0 || config.maximumBlock > 65536 || config.inputs > 256 || config.outputs > 256 || config.parameterCount > 65536
        || config.programCount == 0 || config.programCount > 4096)
        throw std::runtime_error("Invalid audio transport configuration");
    inputOffset = sizeof(AudioSlotHeader);
    outputOffset = inputOffset + size_t(config.inputs) * quantum * sizeof(float);
    inputMidiOffset = alignUp(outputOffset + size_t(config.outputs) * quantum * sizeof(float), alignof(SharedMidi));
    outputMidiOffset = inputMidiOffset + sizeof(SharedMidi);
    transportOffset = outputMidiOffset + sizeof(SharedMidi);
    parameterOffset = transportOffset + sizeof(SharedTransport);
    slotBytes = alignUp(parameterOffset + sizeof(SharedParameters), 64);
    slotOffset = alignUp(sizeof(SharedAudioHeader) + config.parameterCount * sizeof(LONG64), 64);
    totalBytes = slotOffset + audioSlotCount * slotBytes;
    if (totalBytes > 256 * 1024 * 1024) throw std::runtime_error("Audio transport exceeds 256 MiB capacity");
}
Result<std::unique_ptr<SharedAudio>> SharedAudio::map(const std::wstring& name, const ProcessConfig& config, bool creating)
{
    try
    {
        auto result = std::unique_ptr<SharedAudio>(new SharedAudio(name, config));
        if (creating)
        {
            result->mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(result->totalBytes), name.c_str()));
            if (GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("Audio mapping identity already exists");
            result->ready.reset(CreateEventW(nullptr, FALSE, FALSE, (name + L"-ready").c_str()));
            result->done.reset(CreateEventW(nullptr, FALSE, FALSE, (name + L"-done").c_str()));
        }
        else
        {
            result->mapping.reset(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str()));
            result->ready.reset(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + L"-ready").c_str()));
            result->done.reset(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + L"-done").c_str()));
        }
        if (!result->mapping.valid() || !result->ready.valid() || !result->done.valid()) throw std::runtime_error("Cannot open shared audio resources");
        result->view = MapViewOfFile(result->mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, result->totalBytes);
        if (!result->view) throw std::runtime_error("Cannot map shared audio storage");
        if (creating)
        {
            std::memset(result->view, 0, result->totalBytes);
            auto& header = result->header();
            header.magic = 0x4154594e; header.version = 6; header.quantum = result->quantum;
            header.inputs = config.inputs; header.outputs = config.outputs; header.slots = audioSlotCount; header.generation = 1;
            header.parameterCount = config.parameterCount;
            header.programCount = config.programCount;
        }
        auto& header = result->header();
        if (header.magic != 0x4154594e || header.version != 6 || header.quantum != result->quantum
            || header.inputs != config.inputs || header.outputs != config.outputs || header.slots != audioSlotCount
            || header.parameterCount != config.parameterCount || header.programCount != config.programCount)
            throw std::runtime_error("Incompatible audio mapping layout");
        return result;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
Result<std::unique_ptr<SharedAudio>> SharedAudio::create(const std::wstring& name, const ProcessConfig& config) { return map(name, config, true); }
Result<std::unique_ptr<SharedAudio>> SharedAudio::open(const std::wstring& name, const ProcessConfig& config) { return map(name, config, false); }
SharedAudio::~SharedAudio() { if (view) UnmapViewOfFile(view); }
AudioSlotHeader& SharedAudio::slot(uint32_t index) const { return *reinterpret_cast<AudioSlotHeader*>(static_cast<std::byte*>(view) + slotOffset + index * slotBytes); }
float* SharedAudio::input(uint32_t index, uint32_t channel) const { return reinterpret_cast<float*>(reinterpret_cast<std::byte*>(&slot(index)) + inputOffset) + size_t(channel) * quantum; }
float* SharedAudio::output(uint32_t index, uint32_t channel) const { return reinterpret_cast<float*>(reinterpret_cast<std::byte*>(&slot(index)) + outputOffset) + size_t(channel) * quantum; }
SharedMidi& SharedAudio::inputMidi(uint32_t index) const { return *reinterpret_cast<SharedMidi*>(reinterpret_cast<std::byte*>(&slot(index)) + inputMidiOffset); }
SharedMidi& SharedAudio::outputMidi(uint32_t index) const { return *reinterpret_cast<SharedMidi*>(reinterpret_cast<std::byte*>(&slot(index)) + outputMidiOffset); }
SharedTransport& SharedAudio::transportMarkers(uint32_t index) const { return *reinterpret_cast<SharedTransport*>(reinterpret_cast<std::byte*>(&slot(index)) + transportOffset); }
SharedParameters& SharedAudio::parameters(uint32_t index) const { return *reinterpret_cast<SharedParameters*>(reinterpret_cast<std::byte*>(&slot(index)) + parameterOffset); }
uint64_t SharedAudio::parameterSequence(uint32_t index) const
{
    auto* sequences = reinterpret_cast<volatile LONG64*>(static_cast<std::byte*>(view) + sizeof(SharedAudioHeader));
    return static_cast<uint64_t>(InterlockedCompareExchange64(&sequences[index], 0, 0));
}
uint64_t SharedAudio::programSequence() const { return static_cast<uint64_t>(InterlockedCompareExchange64(&header().programSequence, 0, 0)); }
static void acknowledgeSequence(volatile LONG64& target, uint64_t sequence)
{
    auto previous = InterlockedCompareExchange64(&target, 0, 0);
    while (sequence > static_cast<uint64_t>(previous))
    {
        auto observed = InterlockedCompareExchange64(&target, static_cast<LONG64>(sequence), previous);
        if (observed == previous) break;
        previous = observed;
    }
}
void SharedAudio::acknowledge(const ParameterEvent& event) const
{
    if (event.origin >= 3) { acknowledgeSequence(header().programSequence, event.sequence); return; }
    if (event.index >= config.parameterCount) return;
    auto* sequences = reinterpret_cast<volatile LONG64*>(static_cast<std::byte*>(view) + sizeof(SharedAudioHeader));
    acknowledgeSequence(sequences[event.index], event.sequence);
}
void SharedAudio::acknowledgeState(uint64_t sequence) const
{
    acknowledgeSequence(header().programSequence, sequence);
    auto* sequences = reinterpret_cast<volatile LONG64*>(static_cast<std::byte*>(view) + sizeof(SharedAudioHeader));
    for (uint32_t index = 0; index < config.parameterCount; ++index) acknowledgeSequence(sequences[index], sequence);
}

AudioTransport::AudioTransport(std::unique_ptr<SharedAudio> shared)
    : memory(std::move(shared)), midiScratch(std::make_unique<SharedMidi>()), transportScratch(std::make_unique<SharedTransport>()),
      parameterScratch(std::make_unique<SharedParameters>())
{
    inputScratch.resize(size_t(memory->config.inputs) * memory->quantum);
    precisionInput.resize(size_t(memory->config.inputs) * memory->config.maximumBlock);
    precisionOutput.resize(size_t(memory->config.outputs) * memory->config.maximumBlock);
    parameterValues.resize(memory->config.parameterCount);
    parameterSeen.resize(memory->config.parameterCount);
    parameterSequences.resize(memory->config.parameterCount);
}
Result<std::unique_ptr<AudioTransport>> AudioTransport::create(const std::wstring& name, const ProcessConfig& config)
{
    auto shared = SharedAudio::create(name, config);
    if (auto* failure = std::get_if<Failure>(&shared)) return *failure;
    try { return std::unique_ptr<AudioTransport>(new AudioTransport(std::get<std::unique_ptr<SharedAudio>>(std::move(shared)))); }
    catch (const std::exception& error) { return Failure{ErrorCode::io, error.what()}; }
}
void AudioTransport::reset() noexcept
{
    epoch = clock;
    filled = 0;
    midiScratch->count = midiScratch->used = 0;
    transportScratch->count = 0;
    parameterScratch->count = 0;
    if (programEvent && (!programEvent->sequence || programEvent->sequence > memory->programSequence()))
    {
        auto event = *programEvent; event.offset = 0; event.origin = 4;
        if (!parameterScratch->push(event)) InterlockedExchange(&memory->header().error, 1);
    }
    for (uint32_t index = 0; index < parameterSeen.size(); ++index)
        if (parameterSeen[index] && (!parameterSequences[index] || parameterSequences[index] > memory->parameterSequence(index))
            && !parameterScratch->push({index, 0, parameterValues[index], 2, parameterSequences[index]}))
            InterlockedExchange(&memory->header().error, 1);
    outputSequence = UINT64_MAX;
    outputSlot = -1;
    ++generation;
    InterlockedExchange(&memory->header().generation, static_cast<LONG>(generation));
    for (uint32_t i = 0; i < audioSlotCount; ++i) InterlockedCompareExchange(&memory->slot(i).state, 0, 1);
    reap(0);
    pendingNotesOff = true;
}
void AudioTransport::reap(uint64_t before) noexcept
{
    for (uint32_t i = 0; i < audioSlotCount; ++i)
    {
        auto& slot = memory->slot(i);
        if (InterlockedCompareExchange(&slot.state, 0, 0) == 3 && (slot.generation != generation || slot.sequence < before))
            InterlockedExchange(&slot.state, 0);
    }
}
bool AudioTransport::submit() noexcept
{
    for (uint32_t i = 0; i < audioSlotCount; ++i)
    {
        auto& slot = memory->slot(i);
        if (InterlockedCompareExchange(&slot.state, 4, 0) != 0) continue;
        slot.generation = generation;
        slot.sequence = static_cast<uint64_t>((inputTransport.streamSamples - epoch) / memory->quantum);
        slot.transport = inputTransport;
        slot.frames = memory->quantum;
        slot.error = 0;
        for (uint32_t channel = 0; channel < memory->config.inputs; ++channel)
            std::copy_n(inputScratch.data() + size_t(channel) * memory->quantum, memory->quantum, memory->input(i, channel));
        auto& midi = memory->inputMidi(i);
        midi.count = midiScratch->count; midi.used = midiScratch->used;
        std::copy_n(midiScratch->events.data(), midi.count, midi.events.data());
        std::copy_n(midiScratch->bytes.data(), midi.used, midi.bytes.data());
        memory->outputMidi(i).count = memory->outputMidi(i).used = 0;
        auto& markers = memory->transportMarkers(i);
        markers.count = transportScratch->count;
        std::copy_n(transportScratch->markers.data(), markers.count, markers.markers.data());
        transportScratch->count = 0;
        auto& parameters = memory->parameters(i);
        parameters.count = parameterScratch->count;
        std::copy_n(parameterScratch->events.data(), parameters.count, parameters.events.data());
        parameterScratch->count = 0;
        midiScratch->count = midiScratch->used = 0;
        filled = 0;
        InterlockedExchange(&slot.state, 1);
        SetEvent(memory->readyEvent());
        return true;
    }
    InterlockedExchange(&memory->header().error, 1);
    return false;
}
int AudioTransport::findOutput(uint64_t sequence, bool offline) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;)
    {
        for (uint32_t i = 0; i < audioSlotCount; ++i)
        {
            auto& slot = memory->slot(i);
            if (InterlockedCompareExchange(&slot.state, 0, 0) == 3 && slot.generation == generation && slot.sequence == sequence)
                return slot.error == 0 && slot.frames == memory->quantum && memory->outputMidi(i).valid() ? static_cast<int>(i) : -2;
        }
        if (InterlockedCompareExchange(&memory->header().error, 0, 0)) return -2;
        if (!offline) return -1;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return -2;
        HANDLE events[] {memory->doneEvent(), stopEvent};
        auto result = WaitForMultipleObjects(stopEvent ? 2 : 1, events, FALSE, static_cast<DWORD>(remaining));
        if (result != WAIT_OBJECT_0) return -2;
    }
}
ProcessResult AudioTransport::process(AudioBlockView block, EventBlockView events, const TransportSnapshot& transport) noexcept
{
    auto mute = [&]
    {
        for (auto* channel : block.outputs) if (channel) std::fill_n(channel, block.samples, 0.0f);
        return ProcessResult::failed;
    };
    if (block.samples > memory->config.maximumBlock || block.inputs.size() != memory->config.inputs || block.outputs.size() != memory->config.outputs
        || InterlockedCompareExchange(&memory->header().error, 0, 0)) return mute();
    for (auto* input : block.inputs) if (!input && block.samples) return mute();
    for (auto* output : block.outputs) if (!output && block.samples) return mute();
    bool playing = (transport.flags & 2) != 0;
    if (hadTransport && ((playing && wasPlaying && transport.projectSamples != expectedProject) || playing != wasPlaying)) reset();
    hadTransport = true; wasPlaying = playing; expectedProject = transport.projectSamples + block.samples;
    if (pendingNotesOff && events.emit && block.samples > 0)
    {
        for (uint8_t channel = 0; channel < 16; ++channel)
        {
            std::array<std::byte, 3> message {static_cast<std::byte>(0xb0 | channel), std::byte{123}, std::byte{0}};
            events.emit(events.context, clock, message);
        }
        pendingNotesOff = false;
    }
    for (const auto& event : events.input)
        if (event.sample < clock || event.sample >= clock + std::max<uint32_t>(block.samples, 1)) return mute();
    for (const auto& event : events.parameters)
    {
        if (event.offset >= std::max<uint32_t>(block.samples, 1) || !std::isfinite(event.normalized) || event.normalized < 0
            || event.normalized > 1 || event.origin < 1 || event.origin > 4) return mute();
        if (event.origin >= 3)
        {
            if (event.index >= memory->config.programCount || event.normalized != 0) return mute();
            if (!programEvent || !event.sequence || event.sequence >= programEvent->sequence) programEvent = event;
            continue;
        }
        if (event.index >= parameterValues.size()) return mute();
        if (!event.sequence || event.sequence >= parameterSequences[event.index])
        {
            parameterValues[event.index] = event.normalized;
            parameterSequences[event.index] = event.sequence;
        }
        parameterSeen[event.index] = 1;
    }
    if (block.samples == 0)
    {
        for (const auto& event : events.input) if (!midiScratch->push(event.sample, event.data)) return mute();
        for (auto event : events.parameters)
        {
            event.offset = filled;
            if (!parameterScratch->push(event)) return mute();
        }
        return ProcessResult::priming;
    }
    bool rendered = false, underrun = false;
    uint32_t processed = 0;
    while (processed < block.samples)
    {
        const auto quantum = memory->quantum;
        uint32_t amount = std::min(block.samples - processed, quantum - filled);
        auto current = clock + processed;
        auto relative = static_cast<uint64_t>(current - epoch);
        auto nextTransport = transport;
        nextTransport.streamSamples = current;
        if (playing) nextTransport.projectSamples += processed;
        if (playing && (nextTransport.flags & 512)) nextTransport.ppq += processed * nextTransport.tempo / (60.0 * memory->config.sampleRate);
        if (memory->config.offline) nextTransport.processFlags |= 1;
        if (filled == 0)
        {
            inputTransport = nextTransport;
        }
        else
        {
            const auto& previous = transportScratch->count ? transportScratch->markers[transportScratch->count - 1].snapshot : inputTransport;
            auto difference = nextTransport.streamSamples - previous.streamSamples;
            auto expectedPpq = previous.ppq + (playing ? difference * previous.tempo / (60.0 * memory->config.sampleRate) : 0);
            bool changed = nextTransport.flags != previous.flags || nextTransport.processFlags != previous.processFlags
                || nextTransport.tempo != previous.tempo || nextTransport.numerator != previous.numerator || nextTransport.denominator != previous.denominator
                || nextTransport.barStart != previous.barStart || nextTransport.cycleStart != previous.cycleStart || nextTransport.cycleEnd != previous.cycleEnd
                || ((nextTransport.flags & 512) && std::abs(nextTransport.ppq - expectedPpq) > 0.0000001);
            if (changed)
            {
                if (transportScratch->count == transportScratch->markers.size()) return mute();
                transportScratch->markers[transportScratch->count++] = {filled, 0, nextTransport};
            }
        }
        for (const auto& event : events.input)
            if (event.sample >= current && event.sample < current + amount && !midiScratch->push(event.sample, event.data)) return mute();
        for (auto event : events.parameters)
            if (event.offset >= processed && event.offset < processed + amount)
            {
                event.offset = filled + event.offset - processed;
                if (!parameterScratch->push(event)) return mute();
            }
        for (uint32_t channel = 0; channel < memory->config.inputs; ++channel)
            std::copy_n(block.inputs[channel] + processed, amount, inputScratch.data() + size_t(channel) * quantum + filled);
        uint32_t offset = filled;
        filled += amount;
        if (filled == quantum && !submit()) return mute();
        if (relative < uint64_t(quantum) * 2)
        {
            for (auto* output : block.outputs) std::fill_n(output + processed, amount, 0.0f);
        }
        else
        {
            rendered = true;
            auto sequence = relative / quantum - 2;
            if (outputSequence != sequence)
            {
                reap(sequence);
                outputSequence = sequence;
                outputSlot = findOutput(sequence, memory->config.offline || (transport.processFlags & 5));
                if (outputSlot == -2) return mute();
                if (outputSlot < 0) { InterlockedIncrement(&memory->header().underruns); underrun = true; }
            }
            underrun |= outputSlot < 0;
            for (uint32_t channel = 0; channel < memory->config.outputs; ++channel)
            {
                if (outputSlot >= 0) std::copy_n(memory->output(static_cast<uint32_t>(outputSlot), channel) + offset, amount, block.outputs[channel] + processed);
                else std::fill_n(block.outputs[channel] + processed, amount, 0.0f);
            }
            if (outputSlot >= 0 && events.emit)
            {
                auto& midi = memory->outputMidi(static_cast<uint32_t>(outputSlot));
                for (uint32_t index = 0; index < midi.count; ++index)
                {
                    const auto& event = midi.events[index];
                    auto time = event.sample + latency();
                    if (time >= current && time < current + amount)
                        events.emit(events.context, time, std::span(midi.bytes).subspan(event.offset, event.length));
                }
            }
            if (offset + amount == quantum && outputSlot >= 0)
                InterlockedExchange(&memory->slot(static_cast<uint32_t>(outputSlot)).state, 0);
        }
        processed += amount;
    }
    clock += block.samples;
    return underrun ? ProcessResult::underrun : (rendered ? ProcessResult::rendered : ProcessResult::priming);
}
ProcessResult AudioTransport::process(DoubleAudioBlockView block, EventBlockView events, const TransportSnapshot& transport) noexcept
{
    auto mute = [&]
    {
        for (auto* output : block.outputs) if (output) std::fill_n(output, block.samples, 0.0);
        return ProcessResult::failed;
    };
    if (block.samples > memory->config.maximumBlock || block.inputs.size() != memory->config.inputs || block.outputs.size() != memory->config.outputs)
        return mute();
    for (auto* input : block.inputs) if (!input && block.samples) return mute();
    for (auto* output : block.outputs) if (!output && block.samples) return mute();
    std::array<const float*, 256> inputs;
    std::array<float*, 256> outputs;
    for (uint32_t channel = 0; channel < memory->config.inputs; ++channel)
    {
        auto* destination = precisionInput.data() + size_t(channel) * memory->config.maximumBlock;
        for (uint32_t i = 0; i < block.samples; ++i) destination[i] = static_cast<float>(block.inputs[channel][i]);
        inputs[channel] = destination;
    }
    for (uint32_t channel = 0; channel < memory->config.outputs; ++channel)
        outputs[channel] = precisionOutput.data() + size_t(channel) * memory->config.maximumBlock;
    auto result = process(AudioBlockView{std::span(inputs).first(memory->config.inputs), std::span(outputs).first(memory->config.outputs), block.samples}, events, transport);
    for (uint32_t channel = 0; channel < memory->config.outputs; ++channel)
        for (uint32_t i = 0; i < block.samples; ++i) block.outputs[channel][i] = outputs[channel][i];
    return result;
}
std::vector<ParameterEvent> AudioTransport::parameterCheckpoint() const
{
    std::vector<ParameterEvent> checkpoint;
    if (programEvent && (!programEvent->sequence || programEvent->sequence > memory->programSequence()))
    { auto event = *programEvent; event.offset = 0; event.origin = 4; checkpoint.push_back(event); }
    for (uint32_t index = 0; index < parameterSeen.size(); ++index)
        if (parameterSeen[index] && (!parameterSequences[index] || parameterSequences[index] > memory->parameterSequence(index)))
            checkpoint.push_back({index, 0, parameterValues[index], 2, parameterSequences[index]});
    return checkpoint;
}
void AudioTransport::skipSamples(uint64_t count) noexcept
{
    clock += static_cast<int64_t>(count);
    reset();
}
void AudioTransport::restoreCheckpoint(uint32_t program, std::span<const float> values, uint64_t sequence)
{
    if (values.size() != parameterValues.size()) throw std::runtime_error("Restored parameter count differs from adapter topology");
    std::copy(values.begin(), values.end(), parameterValues.begin());
    std::fill(parameterSeen.begin(), parameterSeen.end(), uint8_t{1});
    std::fill(parameterSequences.begin(), parameterSequences.end(), sequence);
    if (program < memory->config.programCount) programEvent = ParameterEvent{program, 0, 0, 4, sequence};
    parameterScratch->count = 0;
    memory->acknowledgeState(sequence);
}
}
