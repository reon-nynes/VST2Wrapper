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
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
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
    std::unique_ptr<struct ExtractedPayload> payload;
    BridgeClient client;
    std::mutex startupMutex;
};
}
