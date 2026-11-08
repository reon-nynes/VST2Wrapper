#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "common/Manifest.h"
#include "bridge/BridgeClient.h"
#include <atomic>
#include <cmath>
namespace nytera
{
class Parameter final : public juce::AudioProcessorParameterWithID
{
public:
    Parameter(const ParameterInfo& info, BridgeClient& bridge)
        : AudioProcessorParameterWithID(juce::ParameterID(juce::String(info.vst3Id), 1), juce::String::fromUTF8(info.name.c_str())),
          descriptor(info), bridge(bridge), value(info.initialValue) {}
    float getValue() const override { return value.load(std::memory_order_relaxed); }
    void setValue(float next) override
    {
        if (!std::isfinite(next)) return;
        next = juce::jlimit(0.0f, 1.0f, next);
        if (value.exchange(next, std::memory_order_relaxed) == next) return;
        displayDirty = true;
        bridge.enqueueParameter({descriptor.index, 0, next, 1});
    }
    float getDefaultValue() const override { return descriptor.initialValue; }
    juce::String getLabel() const override { return juce::String::fromUTF8(descriptor.label.c_str()); }
    bool isAutomatable() const override { return descriptor.automatable; }
    juce::String getText(float next, int length) const override
    {
        juce::String text;
        {
            juce::SpinLock::ScopedTryLockType lock(displayLock);
            if (lock.isLocked() && std::abs(next - displayValue) < 0.000001f) text = displayText;
        }
        if (text.isEmpty()) text = juce::String(next, 4);
        return length > 0 ? text.substring(0, length) : text;
    }
    float getValueForText(const juce::String& text) const override { return juce::jlimit(0.0f, 1.0f, text.getFloatValue()); }
    const ParameterInfo descriptor;
    void beginSourceGesture() { if (!gestureActive) { gestureActive = true; beginChangeGesture(); } }
    void endSourceGesture() { if (gestureActive) { gestureActive = false; endChangeGesture(); } }
    void receiveSource(float next)
    {
        if (!std::isfinite(next)) return;
        next = juce::jlimit(0.0f, 1.0f, next);
        if (value.exchange(next, std::memory_order_relaxed) == next) return;
        displayDirty = true;
        bool implicit = !gestureActive;
        bridge.enqueueParameter({descriptor.index, 0, next, 2});
        if (implicit) beginChangeGesture();
        sendValueChangedMessageToListeners(next);
        if (implicit) endChangeGesture();
    }
    bool needsDisplay() { return displayDirty.exchange(false); }
    void cacheDisplay(const ParameterDisplay& display)
    {
        juce::SpinLock::ScopedLockType lock(displayLock);
        displayValue = display.normalized;
        displayText = juce::String::fromUTF8(display.text.c_str());
    }
private:
    BridgeClient& bridge;
    std::atomic<float> value;
    std::atomic<bool> displayDirty {true};
    bool gestureActive = false;
    mutable juce::SpinLock displayLock;
    juce::String displayText;
    float displayValue = -1;
};
}
