#include "Editor.h"
#include "Processor.h"
#include <windows.h>
namespace nytera
{
Editor::Editor(Processor& owner) : AudioProcessorEditor(owner), processor(owner)
{
    addAndMakeVisible(nativeView);
    setSize(static_cast<int>(moduleBinding().editorSize.width), static_cast<int>(moduleBinding().editorSize.height));
    setWantsKeyboardFocus(true);
}
Editor::~Editor()
{
    stopTimer();
    if (attached) processor.bridge().closeEditor();
    nativeView.setHWND(nullptr);
}
void Editor::paint(juce::Graphics& graphics)
{
    if (failure.isEmpty()) return;
    graphics.fillAll(juce::Colour(0xff202124));
    graphics.setColour(juce::Colours::white);
    graphics.drawFittedText(failure, getLocalBounds().reduced(20), juce::Justification::centred, 8);
}
void Editor::resized() { nativeView.setBounds(getLocalBounds()); }
void Editor::parentHierarchyChanged() { attach(); }
void Editor::visibilityChanged() { if (isShowing()) attach(); }
void Editor::applySize(uint32_t width, uint32_t height)
{
    auto* peer = getPeer();
    double scale = peer ? peer->getPlatformScaleFactor() : 1.0;
    setSize(juce::roundToInt(width / scale), juce::roundToInt(height / scale));
}
void Editor::attach()
{
    if (attached || !getPeer() || !isShowing()) return;
    auto start = processor.ensureStarted();
    if (auto* error = std::get_if<Failure>(&start)) { failure = juce::String::fromUTF8(error->message.c_str()); repaint(); return; }
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, getWidth(), getHeight(), static_cast<HWND>(getPeer()->getNativeHandle()), nullptr, currentModule(), nullptr);
    if (!window) { failure = "Cannot create host editor container"; repaint(); return; }
    nativeView.setHWND(window);
    auto opened = processor.bridge().openEditor(reinterpret_cast<uint64_t>(window), static_cast<float>(getPeer()->getPlatformScaleFactor()));
    if (auto* error = std::get_if<Failure>(&opened))
    {
        nativeView.setHWND(nullptr);
        failure = juce::String::fromUTF8(error->message.c_str());
        repaint();
        return;
    }
    attached = true;
    auto size = processor.bridge().editorSize();
    applySize(size.width, size.height);
    startTimerHz(30);
}
void Editor::timerCallback()
{
    for (const auto& event : processor.bridge().takeNotifications())
        if (event.opcode == 15 && event.index > 0 && event.value > 0)
            applySize(static_cast<uint32_t>(event.index), static_cast<uint32_t>(event.value));
}
}
