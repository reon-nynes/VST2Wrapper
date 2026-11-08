#include "Processor.h"
#include "Parameter.h"
#include "Editor.h"
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
    if (client.processId() != 0) return std::monostate{};
    return Failure{ErrorCode::plugin, "Runtime payload has yet to be packaged"};
}
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new nytera::Processor(); }
