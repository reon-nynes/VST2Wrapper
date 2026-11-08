#include "MainComponent.h"
class NyteraApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Nytera VST2 Wrapper"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String&) override
    {
        window = std::make_unique<Window>(getCommandLineParameterArray());
    }
    void shutdown() override { window.reset(); }
private:
    class Window final : public juce::DocumentWindow
    {
    public:
        explicit Window(const juce::StringArray& arguments) : DocumentWindow("Nytera VST2 Wrapper", juce::Colours::lightgrey, allButtons)
        {
            setUsingNativeTitleBar(true); setContentOwned(new nytera::MainComponent(arguments), true);
            setResizable(true, false); setResizeLimits(720, 520, 1400, 1000); centreWithSize(getWidth(), getHeight()); setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};
START_JUCE_APPLICATION(NyteraApplication)
