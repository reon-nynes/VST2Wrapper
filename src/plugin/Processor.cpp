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
    for (const auto& descriptor : moduleBinding().parameters) addParameter(new Parameter(descriptor));
    setLatencySamples(static_cast<int>(moduleBinding().initialDelay));
}
Processor::~Processor() { client.stop(); }
void Processor::prepareToPlay(double rate, int maximumBlock)
{
    audioReady = false;
    processingPosition = 0;
    inputMidi.reserve(4096);
    outputMidi.ensureSize(2 * (1024 * 1024 + 4096 * 8));
    auto started = ensureStarted();
    if (std::holds_alternative<Failure>(started)) return;
    auto prepared = client.prepare({rate, static_cast<uint32_t>(std::max(1, maximumBlock)),
        static_cast<uint32_t>(moduleBinding().inputs.size()), static_cast<uint32_t>(moduleBinding().outputs.size()), isNonRealtime()});
    audioReady = std::holds_alternative<std::monostate>(prepared);
    if (audioReady) setLatencySamples(static_cast<int>(client.latencySamples()));
}
void Processor::releaseResources()
{
    audioReady = false;
    client.releaseAudio();
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
    if (!audioReady) { buffer.clear(); midi.clear(); return; }
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
    std::lock_guard lock(startupMutex);
    if (client.processId() != 0) return std::monostate{};
    if (!payload)
    {
        auto extracted = ensurePayload(currentModule(), moduleBinding(), defaultCacheRoot());
        if (auto* error = std::get_if<Failure>(&extracted)) return *error;
        payload = std::make_unique<ExtractedPayload>(std::get<ExtractedPayload>(std::move(extracted)));
    }
    return client.start(moduleBinding(), payload->helper, payload->source);
}
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new nytera::Processor(); }
