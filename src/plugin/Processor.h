#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "ModuleBinding.h"
#include "bridge/BridgeClient.h"
#include <mutex>
namespace nytera
{
class Processor final : public juce::AudioProcessor
{
public:
    Processor();
    ~Processor() override;
    const juce::String getName() const override { return juce::String::fromUTF8(moduleDisplayName()); }
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void reset() override { client.resetProcessing(); }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    bool hasEditor() const override { return moduleBinding().hasEditor; }
    juce::AudioProcessorEditor* createEditor() override;
    bool acceptsMidi() const override { return moduleBinding().midiInput; }
    bool producesMidi() const override { return moduleBinding().midiOutput; }
    double getTailLengthSeconds() const override { return moduleBinding().tailSeconds; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    Status ensureStarted();
    BridgeClient& bridge() { return client; }
private:
    static BusesProperties makeBuses();
    template<class Sample> void processBuffer(juce::AudioBuffer<Sample>&, juce::MidiBuffer&, bool bypass = false);
    TransportSnapshot transportSnapshot() const;
    std::unique_ptr<struct ExtractedPayload> payload;
    BridgeClient client;
    std::mutex startupMutex;
    std::vector<MidiEvent> inputMidi;
    juce::MidiBuffer outputMidi;
    int64_t processingPosition = 0;
    bool audioReady = false;
};
}
