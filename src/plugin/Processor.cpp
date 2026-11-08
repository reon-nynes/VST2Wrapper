#include "Processor.h"
#include "Parameter.h"
#include "Editor.h"
#include "package/Cache.h"
namespace nytera
{
juce::AudioProcessor::BusesProperties Processor::makeBuses()
{
    juce::AudioProcessor::BusesProperties buses;
    if (!moduleBinding().inputs.empty()) buses = buses.withInput("Input", juce::AudioChannelSet::canonicalChannelSet(static_cast<int>(moduleBinding().inputs.size())), true);
    if (!moduleBinding().outputs.empty()) buses = buses.withOutput("Output", juce::AudioChannelSet::canonicalChannelSet(static_cast<int>(moduleBinding().outputs.size())), true);
    return buses;
}
Processor::Processor() : AudioProcessor(makeBuses())
{
    for (const auto& descriptor : moduleBinding().parameters) addParameter(new Parameter(descriptor, client));
    for (const auto& name : moduleBinding().programs.names) programNames.add(juce::String::fromUTF8(name.c_str()));
    setLatencySamples(static_cast<int>(moduleBinding().initialDelay));
    startTimerHz(30);
}
Processor::~Processor()
{
    stopTimer();
    std::lock_guard lock(lifecycleMutex);
    for (auto* parameter : getParameters()) static_cast<Parameter*>(parameter)->endSourceGesture();
    client.stop();
}
void Processor::timerCallback()
{
    std::unique_lock lock(lifecycleMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    auto status = client.status();
    if (status.phase == BridgePhase::failed)
    {
        runtimeDiagnostic = juce::String::fromUTF8(status.diagnostic.c_str());
        if (audioReady.exchange(false)) pendingNotesOff = true;
        for (auto* parameter : getParameters()) static_cast<Parameter*>(parameter)->endSourceGesture();
        return;
    }
    if (client.processId() == 0 || runtimeDiagnostic.isNotEmpty()) return;
    if (!audioReady)
    {
        auto flushed = client.flushParameters();
        if (auto* failure = std::get_if<Failure>(&flushed)) { runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str()); return; }
    }
    auto pending = client.takeNotifications();
    if (auto* failure = std::get_if<Failure>(&pending))
    {
        runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str());
        audioReady = false;
        return;
    }
    bool refreshValues = false;
    for (const auto& event : std::get<std::vector<HostNotification>>(pending))
    {
        if (event.index >= 0 && event.index < getParameters().size())
        {
            auto& parameter = *static_cast<Parameter*>(getParameters()[event.index]);
            if (event.opcode == 43) parameter.beginSourceGesture();
            if (event.opcode == 0) parameter.receiveSource(event.normalized);
            if (event.opcode == 44) parameter.endSourceGesture();
        }
        if (event.opcode == 42)
        {
            refreshValues = true;
            updateHostDisplay(ChangeDetails().withProgramChanged(true).withParameterInfoChanged(true).withNonParameterStateChanged(true));
        }
        if (event.opcode == 13)
        {
            setLatencySamples(static_cast<int>(client.latencySamples()));
            tailDuration = std::max(0.0, static_cast<double>(event.normalized));
        }
    }
    if (refreshValues)
    {
        auto programs = client.programInfo();
        if (auto* info = std::get_if<ProgramInfo>(&programs)) syncPrograms(*info, true);
        auto values = client.parameterValues();
        if (auto* current = std::get_if<std::vector<float>>(&values))
            for (size_t i = 0; i < current->size() && i < static_cast<size_t>(getParameters().size()); ++i)
                static_cast<Parameter*>(getParameters()[static_cast<int>(i)])->receiveSource((*current)[i]);
    }
    if (getParameters().isEmpty()) return;
    for (int visited = 0; visited < getParameters().size(); ++visited)
    {
        auto* parameter = static_cast<Parameter*>(getParameters()[static_cast<int>(displayCursor++ % getParameters().size())]);
        if (!parameter->needsDisplay()) continue;
        auto display = client.parameterDisplay(parameter->descriptor.index);
        if (auto* text = std::get_if<ParameterDisplay>(&display)) parameter->cacheDisplay(*text);
        break;
    }
}
void Processor::prepareToPlay(double rate, int maximumBlock)
{
    std::lock_guard lock(lifecycleMutex);
    audioReady = false;
    processingPosition = 0;
    inputMidi.reserve(4096);
    outputMidi.ensureSize(2 * (1024 * 1024 + 4096 * 8));
    lastConfig = ProcessConfig{rate, static_cast<uint32_t>(std::max(1, maximumBlock)),
        static_cast<uint32_t>(moduleBinding().inputs.size()), static_cast<uint32_t>(moduleBinding().outputs.size()), isNonRealtime()};
    auto started = startSource();
    if (auto* failure = std::get_if<Failure>(&started)) { runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str()); return; }
    auto prepared = client.prepare(*lastConfig);
    audioReady = std::holds_alternative<std::monostate>(prepared);
    if (audioReady) { setLatencySamples(static_cast<int>(client.latencySamples())); tailDuration = client.tailLengthSeconds(); }
    else runtimeDiagnostic = juce::String::fromUTF8(std::get<Failure>(prepared).message.c_str());
}
void Processor::releaseResources()
{
    std::lock_guard lock(lifecycleMutex);
    audioReady = false;
    lastConfig.reset();
    if (client.processId()) client.releaseAudio();
}
TransportSnapshot Processor::transportSnapshot() const
{
    TransportSnapshot snapshot;
    snapshot.projectSamples = processingPosition;
    snapshot.streamSamples = processingPosition;
    if (isNonRealtime()) snapshot.processFlags = 1;
    if (auto* playhead = getPlayHead())
    {
        if (auto position = playhead->getPosition())
        {
            if (auto samples = position->getTimeInSamples()) snapshot.projectSamples = *samples;
            if (position->getIsPlaying()) snapshot.flags |= 2;
            if (position->getIsLooping()) snapshot.flags |= 4;
            if (position->getIsRecording()) snapshot.flags |= 8;
            if (auto ppq = position->getPpqPosition()) { snapshot.ppq = *ppq; snapshot.flags |= 512; }
            if (auto tempo = position->getBpm()) { snapshot.tempo = *tempo; snapshot.flags |= 1024; }
            if (auto bar = position->getPpqPositionOfLastBarStart()) { snapshot.barStart = *bar; snapshot.flags |= 2048; }
            if (auto loop = position->getLoopPoints()) { snapshot.cycleStart = loop->ppqStart; snapshot.cycleEnd = loop->ppqEnd; snapshot.flags |= 4096; }
            if (auto signature = position->getTimeSignature())
            {
                snapshot.numerator = signature->numerator; snapshot.denominator = signature->denominator; snapshot.flags |= 8192;
            }
        }
    }
    return snapshot;
}
template<class Sample> void Processor::processBuffer(juce::AudioBuffer<Sample>& buffer, juce::MidiBuffer& midi, bool bypass)
{
    juce::ScopedNoDenormals noDenormals;
    if (!audioReady)
    {
        buffer.clear(); midi.clear();
        if (pendingNotesOff.exchange(false) && producesMidi())
            for (int channel = 1; channel <= 16; ++channel) midi.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
        return;
    }
    inputMidi.clear();
    for (const auto message : midi)
    {
        if (inputMidi.size() == 4096) { buffer.clear(); midi.clear(); return; }
        inputMidi.push_back({processingPosition + message.samplePosition,
            std::span(reinterpret_cast<const std::byte*>(message.data), static_cast<size_t>(message.numBytes))});
    }
    outputMidi.clear();
    struct Sink { juce::MidiBuffer* buffer; int64_t start; int count; } sink {&outputMidi, processingPosition, buffer.getNumSamples()};
    auto emit = [](void* context, int64_t sample, std::span<const std::byte> data)
    {
        auto& target = *static_cast<Sink*>(context);
        auto offset = sample - target.start;
        if (offset >= 0 && offset < target.count)
            target.buffer->addEvent(data.data(), static_cast<int>(data.size()), static_cast<int>(offset));
    };
    auto inputs = std::span<const Sample* const>(buffer.getArrayOfReadPointers(), moduleBinding().inputs.size());
    auto outputs = std::span<Sample* const>(buffer.getArrayOfWritePointers(), moduleBinding().outputs.size());
    ProcessResult result;
    auto transport = transportSnapshot();
    if (bypass) transport.processFlags |= 2;
    if constexpr (std::is_same_v<Sample, double>)
        result = client.process(DoubleAudioBlockView{inputs, outputs, static_cast<uint32_t>(buffer.getNumSamples())}, {inputMidi, &sink, emit}, transport);
    else
        result = client.process(AudioBlockView{inputs, outputs, static_cast<uint32_t>(buffer.getNumSamples())}, {inputMidi, &sink, emit}, transport);
    if (result == ProcessResult::failed) buffer.clear();
    processingPosition += buffer.getNumSamples();
    midi.swapWith(outputMidi);
    if (!producesMidi()) midi.clear();
}
void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    processBuffer(buffer, midi);
}
void Processor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) { processBuffer(buffer, midi); }
void Processor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) { processBuffer(buffer, midi, true); }
void Processor::processBlockBypassed(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) { processBuffer(buffer, midi, true); }
bool Processor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.getMainInputChannels() == static_cast<int>(moduleBinding().inputs.size())
        && layout.getMainOutputChannels() == static_cast<int>(moduleBinding().outputs.size());
}
juce::AudioProcessorEditor* Processor::createEditor() { return new Editor(*this); }
Status Processor::ensureStarted()
{
    std::lock_guard lock(lifecycleMutex);
    return startSource();
}
Status Processor::startSource()
{
    auto status = client.status();
    if (status.phase == BridgePhase::failed) return Failure{ErrorCode::plugin, status.diagnostic};
    if (client.processId() != 0) return std::monostate{};
    if (!payload)
    {
        auto extracted = ensurePayload(currentModule(), moduleBinding(), defaultCacheRoot());
        if (auto* error = std::get_if<Failure>(&extracted)) return *error;
        payload = std::make_unique<ExtractedPayload>(std::get<ExtractedPayload>(std::move(extracted)));
    }
    auto started = client.start(moduleBinding(), payload->helper, payload->source);
    if (auto* failure = std::get_if<Failure>(&started)) return *failure;
    auto flushed = client.flushParameters();
    if (auto* failure = std::get_if<Failure>(&flushed)) return *failure;
    auto values = client.parameterValues();
    if (auto* failure = std::get_if<Failure>(&values)) return *failure;
    const auto& current = std::get<std::vector<float>>(values);
    if (current.size() != static_cast<size_t>(getParameters().size())) return Failure{ErrorCode::identity, "Source parameter topology changed"};
    for (size_t index = 0; index < current.size(); ++index) static_cast<Parameter*>(getParameters()[static_cast<int>(index)])->receiveSource(current[index]);
    auto programs = client.programInfo();
    if (auto* info = std::get_if<ProgramInfo>(&programs)) syncPrograms(*info, true);
    return std::monostate{};
}
Status Processor::retrySource()
{
    std::lock_guard lock(lifecycleMutex);
    audioReady = false;
    if (payload)
    {
        auto checked = ensurePayload(currentModule(), moduleBinding(), defaultCacheRoot());
        if (auto* failure = std::get_if<Failure>(&checked)) return *failure;
        payload = std::make_unique<ExtractedPayload>(std::get<ExtractedPayload>(std::move(checked)));
    }
    auto retried = payload ? client.retry() : startSource();
    if (auto* failure = std::get_if<Failure>(&retried)) { runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str()); return *failure; }
    if (auto restored = client.lastState())
    {
        for (size_t index = 0; index < restored->parameters.size(); ++index)
            static_cast<Parameter*>(getParameters()[static_cast<int>(index)])->receiveSource(restored->parameters[index]);
        currentProgram = static_cast<int>(restored->program);
        updateHostDisplay(ChangeDetails().withProgramChanged(true).withParameterInfoChanged(true));
    }
    if (lastConfig)
    {
        auto prepared = client.prepare(*lastConfig);
        if (auto* failure = std::get_if<Failure>(&prepared)) { runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str()); return *failure; }
        audioReady = true;
        setLatencySamples(static_cast<int>(client.latencySamples())); tailDuration = client.tailLengthSeconds();
    }
    runtimeDiagnostic.clear();
    return std::monostate{};
}
juce::String Processor::diagnostic() const
{
    std::unique_lock lock(lifecycleMutex, std::try_to_lock);
    return lock.owns_lock() ? runtimeDiagnostic : juce::String{};
}
void Processor::setCurrentProgram(int program)
{
    if (program < 0 || program >= getNumPrograms()) return;
    currentProgram = program;
    client.enqueueParameter({static_cast<uint32_t>(program), 0, 0, 3});
}
const juce::String Processor::getProgramName(int program)
{
    if (program < 0 || program >= getNumPrograms()) return {};
    juce::SpinLock::ScopedTryLockType lock(programNameLock);
    return lock.isLocked() ? programNames[program] : juce::String::fromUTF8(moduleBinding().programs.names[static_cast<size_t>(program)].c_str());
}
void Processor::syncPrograms(const ProgramInfo& info, bool mirror)
{
    if (moduleBinding().schemaVersion < 2) return;
    if (info.names.size() != moduleBinding().programs.names.size()) return;
    auto previous = currentProgram.exchange(static_cast<int>(info.current));
    {
        juce::SpinLock::ScopedLockType lock(programNameLock);
        programNames.clear();
        for (const auto& name : info.names) programNames.add(juce::String::fromUTF8(name.c_str()));
    }
    if (mirror && previous != static_cast<int>(info.current)) client.enqueueParameter({info.current, 0, 0, 4});
}
void Processor::cacheState(const StateEnvelope& state)
{
    for (size_t index = 0; index < state.parameters.size(); ++index)
        static_cast<Parameter*>(getParameters()[static_cast<int>(index)])->cacheValue(state.parameters[index]);
    currentProgram = static_cast<int>(std::min(state.program, static_cast<uint32_t>(getNumPrograms() - 1)));
    if (!state.programName.empty() && state.program < static_cast<uint32_t>(getNumPrograms()))
    {
        juce::SpinLock::ScopedLockType lock(programNameLock);
        programNames.set(static_cast<int>(state.program), juce::String::fromUTF8(state.programName.c_str()));
    }
}
void Processor::getStateInformation(juce::MemoryBlock& destination)
{
    std::lock_guard lock(lifecycleMutex);
    auto saved = client.lastState();
    if (client.status().phase != BridgePhase::failed && (!saved || client.processId()))
    {
        auto started = startSource();
        if (auto* failure = std::get_if<Failure>(&started)) runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str());
        else
        {
            auto captured = client.captureState();
            if (auto* state = std::get_if<StateEnvelope>(&captured)) saved = *state;
            else runtimeDiagnostic = juce::String::fromUTF8(std::get<Failure>(captured).message.c_str());
        }
    }
    if (!saved) { destination.setSize(0); return; }
    try
    {
        auto bytes = encodeState(*saved);
        destination.setSize(bytes.size());
        std::memcpy(destination.getData(), bytes.data(), bytes.size());
    }
    catch (const std::exception& error) { runtimeDiagnostic = juce::String::fromUTF8(error.what()); }
}
void Processor::setStateInformation(const void* data, int size)
{
    if (!data || size <= 0) return;
    std::lock_guard lock(lifecycleMutex);
    auto decoded = decodeState({static_cast<const std::byte*>(data), static_cast<size_t>(size)});
    if (auto* failure = std::get_if<Failure>(&decoded)) { runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str()); return; }
    const auto& state = std::get<StateEnvelope>(decoded);
    auto valid = validateStateBinding(state, moduleBinding());
    if (auto* failure = std::get_if<Failure>(&valid)) { runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str()); return; }
    auto restored = client.restoreState(state);
    if (auto* failure = std::get_if<Failure>(&restored)) { runtimeDiagnostic = juce::String::fromUTF8(failure->message.c_str()); audioReady = false; return; }
    cacheState(state);
    if (client.processId())
    {
        auto actual = client.lastState();
        if (actual) cacheState(*actual);
        setLatencySamples(static_cast<int>(client.latencySamples())); tailDuration = client.tailLengthSeconds();
    }
    updateHostDisplay(ChangeDetails().withProgramChanged(true).withParameterInfoChanged(true));
}
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new nytera::Processor(); }
