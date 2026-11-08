#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "Conversion.h"
#include <thread>
#include <atomic>
#include <mutex>
namespace nytera
{
struct PreviewSession;
class MainComponent final : public juce::Component, private juce::Timer, public juce::FileDragAndDropTarget
{
public:
    explicit MainComponent(const juce::StringArray& arguments);
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray&, int, int) override;
private:
    void browse(juce::TextEditor&, bool directory, bool save);
    void inspect();
    void generate(bool preview);
    void beginWork();
    void finishWork(const juce::String&);
    void cancelWork();
    void timerCallback() override;
    RuntimeAssets assets;
    std::optional<InspectionResult> inspected;
    juce::Label heading, instruction, sourceLabel, supportLabel, outputLabel, status;
    juce::TextEditor sourcePath, supportPath, outputPath, report;
    juce::TextButton sourceBrowse {"Choose DLL"}, supportBrowse {"Support folder"}, outputBrowse {"Destination"};
    juce::TextButton inspectButton {"Inspect"}, previewButton {"Test original editor"}, generateButton {"Generate VST3"}, cancelButton {"Cancel"};
    double displayedProgress = 0;
    juce::ProgressBar progress {displayedProgress};
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::DocumentWindow> previewWindow;
    std::jthread worker;
    std::atomic<bool> cancelled {false};
    std::atomic<double> workProgress {0};
    std::mutex workMutex;
    std::string workMessage;
    std::shared_ptr<PreviewSession> activePreview;
    bool busy = false;
};
}
