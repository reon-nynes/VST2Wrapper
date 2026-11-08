#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "common/Manifest.h"
#include <atomic>
namespace nytera
{
class Parameter final : public juce::AudioProcessorParameterWithID
{
public:
    explicit Parameter(const ParameterInfo& info)
        : AudioProcessorParameterWithID(juce::ParameterID(juce::String(info.vst3Id), 1), juce::String::fromUTF8(info.name.c_str())),
          descriptor(info), value(info.initialValue) {}
    float getValue() const override { return value.load(std::memory_order_relaxed); }
    void setValue(float next) override { value.store(juce::jlimit(0.0f, 1.0f, next), std::memory_order_relaxed); }
    float getDefaultValue() const override { return descriptor.initialValue; }
    juce::String getLabel() const override { return juce::String::fromUTF8(descriptor.label.c_str()); }
    bool isAutomatable() const override { return descriptor.automatable; }
    juce::String getText(float next, int length) const override { return juce::String(next, 4).substring(0, length); }
    float getValueForText(const juce::String& text) const override { return juce::jlimit(0.0f, 1.0f, text.getFloatValue()); }
    const ParameterInfo descriptor;
private:
    std::atomic<float> value;
};
}
