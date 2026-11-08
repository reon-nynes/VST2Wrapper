#include "MainComponent.h"
#include "bridge/BridgeClient.h"
#include "package/Cache.h"
namespace nytera
{
static std::filesystem::path pathOf(const juce::String& text) { return std::filesystem::path(text.toWideCharPointer()); }
struct PreviewSession
{
    BridgeClient bridge;
    std::optional<ExtractedPayload> payload;
    Manifest manifest;
};
class PreviewComponent final : public juce::Component
{
public:
    explicit PreviewComponent(std::shared_ptr<PreviewSession> session) : session(std::move(session))
    {
        addAndMakeVisible(view);
        setSize(static_cast<int>(this->session->manifest.editorSize.width), static_cast<int>(this->session->manifest.editorSize.height));
    }
    ~PreviewComponent() override { session->bridge.closeEditor(); view.setHWND(nullptr); }
    void resized() override { view.setBounds(getLocalBounds()); }
    void parentHierarchyChanged() override
    {
        if (view.getHWND() || !getPeer()) return;
        auto parent = static_cast<HWND>(getPeer()->getNativeHandle());
        HWND window = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, getWidth(), getHeight(), parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        view.setHWND(window);
        auto result = session->bridge.openEditor(reinterpret_cast<uint64_t>(window));
        if (auto* failure = std::get_if<Failure>(&result))
        {
            error = juce::String::fromUTF8(failure->message.c_str()); view.setVisible(false); repaint(); return;
        }
        auto size = session->bridge.editorSize(); auto scale = getPeer()->getPlatformScaleFactor();
        setSize(juce::roundToInt(size.width / scale), juce::roundToInt(size.height / scale));
    }
    void paint(juce::Graphics& graphics) override
    {
        if (error.isEmpty()) return;
        graphics.fillAll(juce::Colours::white); graphics.setColour(juce::Colours::black);
        graphics.drawFittedText(error, getLocalBounds().reduced(16), juce::Justification::centred, 8);
    }
private:
    std::shared_ptr<PreviewSession> session;
    juce::HWNDComponent view;
    juce::String error;
};
class PreviewWindow final : public juce::DocumentWindow
{
public:
    explicit PreviewWindow(std::shared_ptr<PreviewSession> session)
        : DocumentWindow(juce::String::fromUTF8(session->manifest.displayName.c_str()), juce::Colours::white, closeButton)
    {
        setUsingNativeTitleBar(true); setContentOwned(new PreviewComponent(std::move(session)), true);
        centreWithSize(getWidth(), getHeight()); setVisible(true);
    }
    void closeButtonPressed() override { setVisible(false); clearContentComponent(); }
};
MainComponent::MainComponent(const juce::StringArray& arguments)
{
    auto application = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    assets = RuntimeAssets::fromDirectory(pathOf(application.getFullPathName()));
    for (auto* component : std::initializer_list<juce::Component*>{&heading, &instruction, &sourceLabel, &supportLabel, &outputLabel,
        &status, &sourcePath, &supportPath, &outputPath, &report, &sourceBrowse, &supportBrowse, &outputBrowse,
        &inspectButton, &previewButton, &generateButton, &cancelButton, &progress}) addAndMakeVisible(component);
    heading.setText("Nytera VST2 Wrapper", juce::dontSendNotification);
    heading.setFont(juce::FontOptions(25.0f));
    instruction.setText("Bind one legacy DLL to a single 64-bit VST3 with its original editor.", juce::dontSendNotification);
    sourceLabel.setText("Source DLL", juce::dontSendNotification);
    supportLabel.setText("Support files (optional)", juce::dontSendNotification);
    outputLabel.setText("Output file", juce::dontSendNotification);
    report.setMultiLine(true); report.setReadOnly(true); report.setScrollbarsShown(true);
    report.setText("Choose or drop a VST2 DLL, then inspect it.\n\nEach generated file permanently binds that DLL and its matched runtime.");
    sourceBrowse.onClick = [this] { browse(sourcePath, false, false); };
    supportBrowse.onClick = [this] { browse(supportPath, true, false); };
    outputBrowse.onClick = [this] { browse(outputPath, false, true); };
    sourcePath.onTextChange = supportPath.onTextChange = [this] { inspected.reset(); };
    inspectButton.onClick = [this] { inspect(); };
    previewButton.onClick = [this] { generate(true); };
    generateButton.onClick = [this] { generate(false); };
    cancelButton.onClick = [this] { cancelWork(); };
    cancelButton.setEnabled(false);
    setSize(800, 610); startTimerHz(15);
    for (int index = 0; index + 1 < arguments.size(); ++index)
    {
        if (arguments[index] == "--source") sourcePath.setText(arguments[++index]);
        else if (arguments[index] == "--output") outputPath.setText(arguments[++index]);
    }
    if (sourcePath.getText().isNotEmpty())
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this)] { if (safe) safe->inspect(); });
}
MainComponent::~MainComponent()
{
    stopTimer(); cancelWork();
    if (worker.joinable()) worker.join();
    previewWindow.reset();
}
void MainComponent::paint(juce::Graphics& graphics) { graphics.fillAll(juce::Colour(0xff20292e)); }
void MainComponent::resized()
{
    heading.setBounds(22, 15, getWidth() - 44, 34); instruction.setBounds(22, 52, getWidth() - 44, 25);
    auto row = [&](int y, juce::Label& label, juce::TextEditor& path, juce::TextButton& button)
    {
        label.setBounds(22, y, 155, 28); path.setBounds(181, y, getWidth() - 340, 28); button.setBounds(getWidth() - 147, y, 125, 28);
    };
    row(93, sourceLabel, sourcePath, sourceBrowse); row(133, supportLabel, supportPath, supportBrowse); row(173, outputLabel, outputPath, outputBrowse);
    report.setBounds(22, 220, getWidth() - 44, getHeight() - 344);
    progress.setBounds(22, getHeight() - 111, getWidth() - 44, 18); status.setBounds(22, getHeight() - 86, getWidth() - 44, 28);
    inspectButton.setBounds(22, getHeight() - 46, 110, 28); previewButton.setBounds(144, getHeight() - 46, 170, 28);
    generateButton.setBounds(getWidth() - 295, getHeight() - 46, 150, 28); cancelButton.setBounds(getWidth() - 133, getHeight() - 46, 111, 28);
}
void MainComponent::browse(juce::TextEditor& target, bool directory, bool save)
{
    chooser = std::make_unique<juce::FileChooser>(directory ? "Choose support folder" : save ? "Save converted VST3" : "Choose VST2 DLL",
        juce::File(target.getText()), directory ? "*" : save ? "*.vst3" : "*.dll");
    auto browserFlags = directory ? juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
        : save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting
        : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync(browserFlags, [safe = juce::Component::SafePointer<MainComponent>(this), &target](const juce::FileChooser& chosen)
    {
        if (!safe || chosen.getResult() == juce::File{}) return;
        target.setText(chosen.getResult().getFullPathName());
    });
}
bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files) { return !busy && files.size() == 1 && files[0].endsWithIgnoreCase(".dll"); }
void MainComponent::filesDropped(const juce::StringArray& files, int, int) { if (isInterestedInFileDrag(files)) { sourcePath.setText(files[0]); inspect(); } }
void MainComponent::beginWork()
{
    busy = true; cancelled = false; workProgress = 0;
    status.setText("Working with the matched source helper", juce::dontSendNotification);
    for (auto* component : std::initializer_list<juce::Component*>{&sourcePath, &supportPath, &outputPath, &sourceBrowse, &supportBrowse,
        &outputBrowse, &inspectButton, &previewButton, &generateButton}) component->setEnabled(false);
    cancelButton.setEnabled(true);
}
void MainComponent::finishWork(const juce::String& message)
{
    busy = false;
    for (auto* component : std::initializer_list<juce::Component*>{&sourcePath, &supportPath, &outputPath, &sourceBrowse, &supportBrowse,
        &outputBrowse, &inspectButton, &previewButton, &generateButton}) component->setEnabled(true);
    cancelButton.setEnabled(false); status.setText(message, juce::dontSendNotification);
    { std::lock_guard lock(workMutex); workMessage.clear(); activePreview.reset(); }
}
void MainComponent::cancelWork()
{
    cancelled = true;
    std::shared_ptr<PreviewSession> current;
    { std::lock_guard lock(workMutex); current = activePreview; }
    if (current) current->bridge.stop();
}
void MainComponent::timerCallback()
{
    displayedProgress = workProgress.load();
    std::lock_guard lock(workMutex);
    if (busy && !workMessage.empty()) status.setText(juce::String::fromUTF8(workMessage.c_str()), juce::dontSendNotification);
}
void MainComponent::inspect()
{
    if (busy) return;
    auto source = pathOf(sourcePath.getText()), support = pathOf(supportPath.getText());
    beginWork();
    worker = std::jthread([this, source, support]
    {
        auto result = inspectSource(source, assets, support, [this] { return cancelled.load(); });
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this), result = std::move(result)]() mutable
        {
            if (!safe) return;
            if (auto* failure = std::get_if<Failure>(&result)) { safe->report.setText(juce::String::fromUTF8(failure->message.c_str())); safe->finishWork("Inspection failed"); return; }
            safe->inspected = std::get<InspectionResult>(std::move(result));
            safe->report.setText(juce::String::fromUTF8(safe->inspected->report.c_str()));
            if (safe->outputPath.getText().isEmpty())
                safe->outputPath.setText(juce::File(safe->sourcePath.getText()).getSiblingFile(juce::String::fromUTF8(outputFilename(safe->inspected->manifest).c_str())).getFullPathName());
            safe->workProgress = 1; safe->finishWork("Inspection complete");
        });
    });
}
void MainComponent::generate(bool preview)
{
    if (busy) return;
    if (sourcePath.getText().isEmpty() || (!preview && outputPath.getText().isEmpty())) { status.setText("Choose a source DLL and destination file.", juce::dontSendNotification); return; }
    ConversionRequest request {pathOf(sourcePath.getText()), pathOf(supportPath.getText()), pathOf(outputPath.getText()), inspected};
    beginWork();
    worker = std::jthread([this, request = std::move(request), preview]() mutable
    {
        auto temporary = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("NyteraPreview", ".vst3", false);
        if (preview) request.output = pathOf(temporary.getFullPathName());
        auto result = generateConversion(request, assets, [this](double value, const std::string& message)
        {
            workProgress = value; { std::lock_guard lock(workMutex); workMessage = message; } return !cancelled;
        });
        std::shared_ptr<PreviewSession> session;
        if (preview && std::holds_alternative<std::monostate>(result))
        {
            session = std::make_shared<PreviewSession>();
            { std::lock_guard lock(workMutex); activePreview = session; }
            auto metadata = readPackageManifest(request.output);
            if (auto* failure = std::get_if<Failure>(&metadata)) result = *failure;
            else
            {
                session->manifest = std::get<Manifest>(metadata);
                HMODULE module = LoadLibraryExW(request.output.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
                auto extracted = ensurePayload(module, session->manifest, defaultCacheRoot()); FreeLibrary(module);
                if (auto* extractionFailure = std::get_if<Failure>(&extracted)) result = *extractionFailure;
                else
                {
                    session->payload = std::get<ExtractedPayload>(std::move(extracted));
                    if (cancelled) result = Failure{ErrorCode::disconnected, "Preview cancelled"};
                    else result = session->bridge.start(session->manifest, session->payload->helper, session->payload->source, [this] { return cancelled.load(); });
                }
            }
        }
        if (preview) temporary.deleteFile();
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this), result = std::move(result), session, preview]
        {
            if (!safe) return;
            if (auto* failure = std::get_if<Failure>(&result)) { safe->report.setText(juce::String::fromUTF8(failure->message.c_str())); safe->finishWork("Operation failed"); return; }
            if (preview && !safe->cancelled) safe->previewWindow = std::make_unique<PreviewWindow>(session);
            safe->finishWork(preview ? "Original editor preview" : "Single-file VST3 generated");
        });
    });
}
}
