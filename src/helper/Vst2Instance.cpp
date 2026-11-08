#include "Vst2Instance.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <limits>
#include <bit>
namespace nytera
{
static void trace(const char* stage)
{
    char enabled[2] {};
    if (GetEnvironmentVariableA("NYTERA_TRACE", enabled, sizeof(enabled))) std::clog << stage << std::endl;
}
thread_local Vst2Instance* Vst2Instance::constructing = nullptr;
thread_local Vst2Instance* Vst2Instance::processing = nullptr;
thread_local Vst2Instance* Vst2Instance::applyingParameter = nullptr;
thread_local const TransportSnapshot* Vst2Instance::processTransport = nullptr;
thread_local EventBlockView* Vst2Instance::processEvents = nullptr;
Result<std::unique_ptr<Vst2Instance>> Vst2Instance::open(const std::filesystem::path& path)
{
    auto architecture = inspectPeArchitecture(path);
    if (auto* failure = std::get_if<Failure>(&architecture)) return *failure;
    constexpr auto nativeArchitecture = sizeof(void*) == 8 ? Architecture::x64 : Architecture::x86;
    if (std::get<Architecture>(architecture) != nativeArchitecture)
        return Failure{ErrorCode::architecture, "DLL architecture does not match helper architecture"};
    auto instance = std::unique_ptr<Vst2Instance>(new Vst2Instance);
    try
    {
        instance->source = std::filesystem::absolute(path);
        instance->sourceDirectory = instance->source.parent_path().string();
        trace("Loading DLL");
        instance->module = LoadLibraryExW(instance->source.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!instance->module) return Failure{ErrorCode::plugin, "Windows could not load DLL or dependency (error " + std::to_string(GetLastError()) + ")"};
        auto entry = reinterpret_cast<Vst2Entry>(GetProcAddress(instance->module, "VSTPluginMain"));
        if (!entry) entry = reinterpret_cast<Vst2Entry>(GetProcAddress(instance->module, "main"));
        if (!entry) return Failure{ErrorCode::invalidData, "DLL has no VST2 entry point"};
        auto* previous = constructing;
        constructing = instance.get();
        trace("Calling VST2 entry point");
        try { instance->effect = entry(host); }
        catch (...) { constructing = previous; throw; }
        constructing = previous;
        trace("VST2 entry point returned");
        auto* effect = instance->effect;
        if (!effect || effect->magic_number != VST_FOURCC('V', 's', 't', 'P') || !effect->control)
        {
            instance->effect = nullptr;
            return Failure{ErrorCode::invalidData, "Invalid VST2 instance or magic"};
        }
        effect->host_internal = instance.get();
        if (effect->num_params < 0 || effect->num_params > 65536 || effect->num_inputs < 0
            || effect->num_inputs > 256 || effect->num_outputs < 0 || effect->num_outputs > 256
            || effect->num_programs < 0 || effect->num_programs > 4096
            || (effect->num_params > 0 && (!effect->get_parameter || !effect->set_parameter))
            || (!effect->process_float && !effect->process && !effect->process_double))
            return Failure{ErrorCode::invalidData, "Invalid VST2 counts or function pointers"};
        instance->opened = true;
        trace("Opening VST2 instance");
        instance->dispatch(VST_EFFECT_OPCODE_CREATE);
        trace("Setting initial sample rate");
        instance->dispatch(VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr, 44100.0f);
        trace("Setting initial block size");
        instance->dispatch(VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, 512);
        trace("VST2 instance initialized");
        if (effect->num_params < 0 || effect->num_params > 65536 || effect->num_inputs < 0 || effect->num_inputs > 256
            || effect->num_outputs < 0 || effect->num_outputs > 256 || effect->num_programs < 0 || effect->num_programs > 4096)
            return Failure{ErrorCode::invalidData, "Source topology is invalid after initialization"};
        instance->parameterSequences.resize(static_cast<size_t>(effect->num_params));
        instance->parameterValues.resize(static_cast<size_t>(effect->num_params));
        for (uint32_t i = 0; i < instance->parameterValues.size(); ++i) instance->parameterValues[i] = effect->get_parameter(effect, i);
        return instance;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::plugin, error.what()}; }
}
Vst2Instance::~Vst2Instance() { close(); }
void Vst2Instance::publishTransport(const TransportSnapshot& snapshot) noexcept
{
    auto words = std::bit_cast<std::array<uint64_t, 9>>(snapshot);
    transportSequence.fetch_add(1);
    for (size_t i = 0; i < words.size(); ++i) transportWords[i].store(words[i]);
    transportSequence.fetch_add(1);
}
bool Vst2Instance::readTransport(TransportSnapshot& snapshot) const noexcept
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        auto before = transportSequence.load();
        if (before == 0 || (before & 1)) continue;
        std::array<uint64_t, 9> words;
        for (size_t i = 0; i < words.size(); ++i) words[i] = transportWords[i].load();
        if (transportSequence.load() == before) { snapshot = std::bit_cast<TransportSnapshot>(words); return true; }
    }
    return false;
}
Status Vst2Instance::applyParameter(const ParameterEvent& event)
{
    if (event.origin == 3 || event.origin == 4) return setProgram(event.index, event.sequence, event.origin == 4);
    if (!effect || event.index >= static_cast<uint32_t>(effect->num_params) || !std::isfinite(event.normalized)
        || event.normalized < 0 || event.normalized > 1 || (event.origin != 1 && event.origin != 2)
        || event.index >= parameterSequences.size())
        return Failure{ErrorCode::invalidData, "Invalid source parameter event"};
    auto& lastSequence = parameterSequences[event.index];
    // Control-thread changes may overtake an older block in the audio pipeline.
    if (event.sequence && event.sequence <= lastSequence) return std::monostate{};
    if (event.origin == 2 && effect->get_parameter(effect, event.index) == event.normalized)
    { lastSequence = event.sequence; parameterValues[event.index] = event.normalized; return std::monostate{}; }
    auto* previous = applyingParameter;
    applyingParameter = this;
    try { effect->set_parameter(effect, event.index, event.normalized); }
    catch (...) { applyingParameter = previous; throw; }
    applyingParameter = previous;
    lastSequence = event.sequence;
    parameterValues[event.index] = event.normalized;
    return std::monostate{};
}
Result<std::string> Vst2Instance::parameterDisplay(uint32_t index)
{
    if (!effect || index >= static_cast<uint32_t>(effect->num_params))
        return Failure{ErrorCode::invalidData, "Parameter index is out of range"};
    char text[1024] {};
    dispatch(VST_EFFECT_OPCODE_PARAM_VALUE, static_cast<int32_t>(index), 0, text);
    text[sizeof(text) - 1] = 0;
    return std::string(text);
}
Result<Manifest> Vst2Instance::inspect()
{
    trace("Inspecting VST2 metadata");
    if (!effect) return Failure{ErrorCode::plugin, "Plugin is closed"};
    if (!(effect->flags & VST_EFFECT_FLAG_EDITOR)) return Failure{ErrorCode::unsupported, "Plugin has no original editor"};
    if (dispatch(VST_EFFECT_OPCODE_CATEGORY) == VST_EFFECT_CATEGORY_CONTAINER)
        return Failure{ErrorCode::unsupported, "Multi-plugin shell DLLs require shell support"};
    auto text = [&](int32_t opcode, int32_t index = 0)
    {
        char buffer[1024] {};
        dispatch(opcode, index, 0, buffer);
        buffer[sizeof(buffer) - 1] = 0;
        return std::string(buffer);
    };
    try
    {
        Manifest manifest;
        manifest.sourceHash = sha256(readFile(source));
        manifest.dllName = pathUtf8(source.filename());
        manifest.displayName = text(VST_EFFECT_OPCODE_EFFECT_NAME);
        if (manifest.displayName.empty()) manifest.displayName = pathUtf8(source.stem());
        manifest.sourceVendor = text(VST_EFFECT_OPCODE_VENDOR_NAME);
        manifest.sourceUniqueId = static_cast<uint32_t>(effect->unique_id);
        manifest.architecture = sizeof(void*) == 8 ? Architecture::x64 : Architecture::x86;
        manifest.kind = (effect->flags & VST_EFFECT_FLAG_INSTRUMENT) ? PluginKind::instrument : PluginKind::effect;
        manifest.initialDelay = static_cast<uint32_t>(std::max(0, effect->delay));
        manifest.tailSeconds = tailSeconds();
        for (int32_t i = 0; i < effect->num_params; ++i)
        {
            auto value = effect->get_parameter(effect, static_cast<uint32_t>(i));
            if (!std::isfinite(value)) return Failure{ErrorCode::invalidData, "Plugin returned a non-finite parameter"};
            manifest.parameters.push_back({static_cast<uint32_t>(i), 0, text(VST_EFFECT_OPCODE_PARAM_NAME, i),
                text(VST_EFFECT_OPCODE_PARAM_LABEL, i), std::clamp(value, 0.0f, 1.0f), dispatch(VST_EFFECT_OPCODE_PARAM_IS_AUTOMATABLE, i) != 0});
        }
        for (int32_t i = 0; i < effect->num_inputs; ++i) manifest.inputs.push_back({"Input " + std::to_string(i + 1)});
        for (int32_t i = 0; i < effect->num_outputs; ++i) manifest.outputs.push_back({"Output " + std::to_string(i + 1)});
        manifest.midiInput = manifest.kind == PluginKind::instrument || dispatch(VST_EFFECT_OPCODE_SUPPORTS, 0, 0, const_cast<char*>("receiveVstMidiEvent")) > 0;
        manifest.midiOutput = dispatch(VST_EFFECT_OPCODE_SUPPORTS, 0, 0, const_cast<char*>("sendVstMidiEvent")) > 0;
        vst_rect_t* rectangle = nullptr;
        dispatch(VST_EFFECT_OPCODE_EDITOR_GET_RECT, 0, 0, &rectangle);
        if (rectangle && rectangle->right > rectangle->left && rectangle->bottom > rectangle->top)
            manifest.editorSize = {static_cast<uint32_t>(rectangle->right - rectangle->left), static_cast<uint32_t>(rectangle->bottom - rectangle->top)};
        auto programs = programInfo();
        if (auto* failure = std::get_if<Failure>(&programs)) return *failure;
        manifest.programs = std::get<ProgramInfo>(std::move(programs));
        manifest = assignBindingIds(std::move(manifest));
        binding = manifest;
        return manifest;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::plugin, error.what()}; }
}
Status Vst2Instance::prepare(const ProcessConfig& next)
{
    if (!effect) return Failure{ErrorCode::plugin, "Plugin is closed"};
    if (!std::isfinite(next.sampleRate) || next.sampleRate < 1000 || next.sampleRate > 768000
        || next.maximumBlock == 0 || next.maximumBlock > 65536)
        return Failure{ErrorCode::invalidData, "Invalid processing configuration"};
    suspend();
    config = next;
    callbackRate = next.sampleRate;
    callbackBlock = next.maximumBlock;
    events = std::make_unique<Vst2Events>();
    midiEvents.resize(4128);
    sysexEvents.resize(4096);
    doubleInputs.resize(size_t(effect->num_inputs) * config.maximumBlock);
    doubleOutputs.resize(size_t(effect->num_outputs) * config.maximumBlock);
    dryLatency = static_cast<uint32_t>(std::max(0, effect->delay));
    if (uint64_t(dryLatency) * std::max(1, effect->num_outputs) * sizeof(float) > 256 * 1024 * 1024)
        return Failure{ErrorCode::unsupported, "Source bypass delay exceeds 256 MiB capacity"};
    dryDelay.assign(size_t(dryLatency) * effect->num_outputs, 0);
    dryPosition = 0;
    dispatch(VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr, static_cast<float>(config.sampleRate));
    dispatch(VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, config.maximumBlock);
    dispatch(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1);
    dispatch(VST_EFFECT_OPCODE_PROCESS_BEGIN);
    prepared = true;
    return refreshLatency();
}
Status Vst2Instance::refreshLatency()
{
    auto delay = static_cast<uint32_t>(std::max(0, effect->delay));
    if (uint64_t(delay) * std::max(1, effect->num_outputs) * sizeof(float) > 256 * 1024 * 1024)
        return Failure{ErrorCode::unsupported, "Source bypass delay exceeds 256 MiB capacity"};
    if (delay != dryLatency)
    {
        dryDelay.assign(size_t(delay) * effect->num_outputs, 0.0f);
        dryLatency = delay;
        dryPosition = 0;
    }
    return std::monostate{};
}
double Vst2Instance::tailSeconds()
{
    auto tail = dispatch(VST_EFFECT_OPCODE_TAIL_SAMPLES);
    if (tail >= INT32_MAX) return std::numeric_limits<double>::infinity();
    return tail > 1 ? static_cast<double>(tail) / config.sampleRate : 0;
}
void Vst2Instance::suspend()
{
    if (!prepared) return;
    resetEvents();
    dispatch(VST_EFFECT_OPCODE_PROCESS_END);
    dispatch(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0);
    prepared = false;
}
Status Vst2Instance::process(AudioBlockView block, EventBlockView midi, const TransportSnapshot& transport)
{
    if (!prepared || block.samples > config.maximumBlock || block.inputs.size() != static_cast<size_t>(effect->num_inputs)
        || block.outputs.size() != static_cast<size_t>(effect->num_outputs) || midi.input.size() > 4096
        || effect->num_params != static_cast<int32_t>(parameterSequences.size()))
        return Failure{ErrorCode::plugin, "Source topology or processing bounds changed"};
    processing = this; processTransport = &transport; processEvents = &midi;
    processingSamples = block.samples; invalidOutputEvents = false;
    publishTransport(transport);
    try
    {
        events->count = 0;
        size_t resetCount = flushNotes ? 32 : 0;
        flushNotes = false;
        for (size_t i = 0; i < resetCount; ++i)
        {
            auto& event = midiEvents[i];
            std::memset(&event, 0, sizeof(event));
            event.event.type = VST_EVENT_TYPE_MIDI;
            event.event.size = sizeof(event);
            event.midi.data[0] = static_cast<char>(0xb0 | (i / 2));
            event.midi.data[1] = static_cast<char>(i % 2 == 0 ? 123 : 120);
            events->events[events->count++] = &event.event;
        }
        for (size_t index = 0; index < midi.input.size(); ++index)
        {
            const auto& message = midi.input[index];
            auto offset = message.sample - transport.streamSamples;
            if (message.data.empty() || offset < 0 || offset >= block.samples) throw std::runtime_error("Invalid MIDI timing");
            if (message.data[0] == std::byte{0xf0})
            {
                auto& event = sysexEvents[index];
                std::memset(&event, 0, sizeof(event));
                event.event.type = VST_EVENT_TYPE_MIDI_SYSEX;
                event.event.size = sizeof(event);
                event.event.offset = static_cast<int32_t>(offset);
                event.sysex.size = static_cast<int32_t>(message.data.size());
                event.sysex.data = reinterpret_cast<char*>(const_cast<std::byte*>(message.data.data()));
                events->events[events->count++] = &event.event;
            }
            else
            {
                if (message.data.size() > 3) throw std::runtime_error("Invalid short MIDI message");
                auto& event = midiEvents[index + resetCount];
                std::memset(&event, 0, sizeof(event));
                event.event.type = VST_EVENT_TYPE_MIDI;
                event.event.size = sizeof(event);
                event.event.offset = static_cast<int32_t>(offset);
                std::memcpy(event.midi.data, message.data.data(), message.data.size());
                events->events[events->count++] = &event.event;
            }
        }
        if (events->count) dispatch(VST_EFFECT_OPCODE_EVENT, 0, 0, events.get());
        if (block.samples) hasProcessed = true;
        std::array<float*, 256> nativeOutputs;
        std::copy(block.outputs.begin(), block.outputs.end(), nativeOutputs.begin());
        for (auto* output : block.outputs) std::fill_n(output, block.samples, 0.0f);
        if (effect->process_float)
            effect->process_float(effect, block.inputs.data(), nativeOutputs.data(), static_cast<int32_t>(block.samples));
        else if (effect->process)
            effect->process(effect, block.inputs.data(), nativeOutputs.data(), static_cast<int32_t>(block.samples));
        else
        {
            std::array<const double*, 256> inputs;
            std::array<double*, 256> outputs;
            for (size_t channel = 0; channel < block.inputs.size(); ++channel)
            {
                auto* target = doubleInputs.data() + channel * config.maximumBlock;
                std::copy_n(block.inputs[channel], block.samples, target);
                inputs[channel] = target;
            }
            for (size_t channel = 0; channel < block.outputs.size(); ++channel)
                outputs[channel] = doubleOutputs.data() + channel * config.maximumBlock;
            effect->process_double(effect, inputs.data(), outputs.data(), static_cast<int32_t>(block.samples));
            for (size_t channel = 0; channel < block.outputs.size(); ++channel)
                for (uint32_t i = 0; i < block.samples; ++i) block.outputs[channel][i] = static_cast<float>(outputs[channel][i]);
        }
        for (uint32_t sample = 0; sample < block.samples; ++sample)
        {
            for (size_t channel = 0; channel < block.outputs.size(); ++channel)
            {
                float dry = channel < block.inputs.size() ? block.inputs[channel][sample] : 0.0f;
                if (dryLatency)
                {
                    auto& delayed = dryDelay[channel * dryLatency + dryPosition];
                    std::swap(dry, delayed);
                }
                if (transport.processFlags & 2) block.outputs[channel][sample] = dry;
            }
            if (dryLatency) dryPosition = (dryPosition + 1) % dryLatency;
        }
        if (invalidOutputEvents) throw std::runtime_error("Source emitted invalid MIDI events");
        processing = nullptr; processTransport = nullptr; processEvents = nullptr;
        return std::monostate{};
    }
    catch (const std::exception& error)
    {
        processing = nullptr; processTransport = nullptr; processEvents = nullptr;
        return Failure{ErrorCode::plugin, error.what()};
    }
}
void Vst2Instance::resetEvents()
{
    flushNotes = hasProcessed;
    std::fill(dryDelay.begin(), dryDelay.end(), 0.0f);
    dryPosition = 0;
}
void Vst2Instance::close() noexcept
{
    if (effect && opened)
    {
        try
        {
            if (prepared) { dispatch(VST_EFFECT_OPCODE_PROCESS_END); dispatch(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0); }
            dispatch(VST_EFFECT_OPCODE_DESTROY);
        }
        catch (...) {}
    }
    effect = nullptr;
    opened = prepared = false;
    if (module) { FreeLibrary(module); module = nullptr; }
}
intptr_t Vst2Instance::dispatch(int32_t opcode, int32_t index, intptr_t value, void* pointer, float real)
{
    return effect->control(effect, opcode, index, value, pointer, real);
}
intptr_t Vst2Instance::host(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t value, void* pointer, float real)
{
    auto* instance = constructing ? constructing : (effect ? static_cast<Vst2Instance*>(effect->host_internal) : nullptr);
    return instance ? instance->hostCall(opcode, index, value, pointer, real) : 0;
}
intptr_t Vst2Instance::hostCall(int32_t opcode, int32_t index, intptr_t value, void* pointer, float real)
{
    switch (opcode)
    {
        case 7:
        {
            thread_local Vst2TimeInfo time;
            time = {};
            time.sampleRate = callbackRate.load();
            TransportSnapshot cached;
            const auto* known = processing == this ? processTransport : (readTransport(cached) ? &cached : nullptr);
            if (known)
            {
                const auto& snapshot = *known;
                time.samplePosition = static_cast<double>(snapshot.projectSamples);
                time.ppq = snapshot.ppq; time.tempo = snapshot.tempo; time.barStart = snapshot.barStart;
                time.cycleStart = snapshot.cycleStart; time.cycleEnd = snapshot.cycleEnd;
                time.numerator = snapshot.numerator; time.denominator = snapshot.denominator;
                time.flags = static_cast<int32_t>(snapshot.flags);
            }
            return reinterpret_cast<intptr_t>(&time);
        }
        case VST_HOST_OPCODE_EVENT:
        {
            if (processing != this || !processEvents || !processEvents->emit || !pointer) return 0;
            auto& list = *static_cast<Vst2Events*>(pointer);
            auto reject = [&]() -> intptr_t { invalidOutputEvents = true; return 0; };
            if (list.count < 0 || list.count > 4128) return reject();
            for (int32_t i = 0; i < list.count; ++i)
            {
                auto* event = list.events[i];
                if (!event || event->offset < 0 || event->offset >= static_cast<int32_t>(processingSamples)) return reject();
                std::span<const std::byte> bytes;
                if (event->type == VST_EVENT_TYPE_MIDI)
                {
                    auto* message = reinterpret_cast<vst_event_midi_t*>(event);
                    auto status = static_cast<uint8_t>(message->midi.data[0]);
                    uint32_t length = 3;
                    if ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 || status == 0xf1 || status == 0xf3) length = 2;
                    else if (status >= 0xf4 || status == 0xf0) length = 1;
                    bytes = {reinterpret_cast<const std::byte*>(message->midi.data), length};
                }
                else if (event->type == VST_EVENT_TYPE_MIDI_SYSEX)
                {
                    auto* message = reinterpret_cast<vst_event_midi_sysex_t*>(event);
                    if (message->sysex.size <= 0 || message->sysex.size > 1024 * 1024 || !message->sysex.data) return reject();
                    bytes = {reinterpret_cast<const std::byte*>(message->sysex.data), static_cast<size_t>(message->sysex.size)};
                }
                if (!bytes.empty()) processEvents->emit(processEvents->context, processTransport->streamSamples + event->offset, bytes);
            }
            return 1;
        }
        case VST_HOST_OPCODE_VST_VERSION: return 2400;
        case VST_HOST_OPCODE_GET_SAMPLE_RATE: return static_cast<intptr_t>(callbackRate.load());
        case VST_HOST_OPCODE_GET_BLOCK_SIZE: return callbackBlock.load();
        case VST_HOST_OPCODE_VENDOR_NAME:
        case VST_HOST_OPCODE_PRODUCT_NAME:
            if (pointer) std::memcpy(pointer, "Nytera", 7);
            return 1;
        case VST_HOST_OPCODE_VENDOR_VERSION: return 100;
        case VST_HOST_OPCODE_LANGUAGE: return 1;
        case VST_HOST_OPCODE_GET_EFFECT_DIRECTORY: return reinterpret_cast<intptr_t>(sourceDirectory.c_str());
        case VST_HOST_OPCODE_GET_ACTIVE_THREAD: return processing == this && processTransport ? ((processTransport->processFlags & 1) ? 4 : 2) : 1;
        case VST_HOST_OPCODE_SUPPORTS:
            if (pointer)
            {
                auto* capability = static_cast<const char*>(pointer);
                for (auto* supported : {"sizeWindow", "sendVstEvents", "sendVstMidiEvent", "receiveVstEvents", "receiveVstMidiEvent", "sendVstTimeInfo", "startStopProcess", "acceptIOChanges"})
                    if (std::strcmp(capability, supported) == 0) return 1;
            }
            return 0;
        case VST_HOST_OPCODE_AUTOMATE:
            if (applyingParameter == this) return 1;
            if (onNotification) onNotification({static_cast<uint32_t>(opcode), index, value, real});
            return 1;
        case VST_HOST_OPCODE_PARAM_START_EDIT:
        case VST_HOST_OPCODE_PARAM_STOP_EDIT:
            if (applyingParameter == this) return 1;
            if (onNotification) onNotification({static_cast<uint32_t>(opcode), index, value, real});
            return 1;
        case VST_HOST_OPCODE_IO_MODIFIED:
        case VST_HOST_OPCODE_EDITOR_RESIZE:
        case VST_HOST_OPCODE_EDITOR_UPDATE:
            if (opcode == VST_HOST_OPCODE_EDITOR_UPDATE && applyingParameter == this) return 1;
            if (onNotification) onNotification({static_cast<uint32_t>(opcode), index, value, real});
            return 1;
        default: return 0;
    }
}
}
