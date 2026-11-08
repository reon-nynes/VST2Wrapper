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
void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();
    midi.clear();
}
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
