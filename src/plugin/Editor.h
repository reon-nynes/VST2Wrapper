#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
namespace nytera
{
class Processor;
class Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Editor(Processor&);
    ~Editor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;
private:
    void attach();
    void applySize(uint32_t width, uint32_t height);
    void timerCallback() override;
    Processor& processor;
    juce::HWNDComponent nativeView;
    juce::String failure;
    juce::TextButton retry {"Retry this plugin"};
    bool attached = false;
};
}
