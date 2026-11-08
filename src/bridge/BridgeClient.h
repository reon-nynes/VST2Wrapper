#pragma once
#include "common/Manifest.h"
#include "common/Processing.h"
#include "common/Wire.h"
#include "common/State.h"
#include <optional>
#include <chrono>
#include <memory>

namespace nytera
{
class BridgeClient
{
public:
    BridgeClient();
    ~BridgeClient();
    BridgeClient(const BridgeClient&) = delete;
    BridgeClient& operator=(const BridgeClient&) = delete;
    Status start(const Manifest&, const std::filesystem::path& helper, const std::filesystem::path& source);
    Result<Bytes> request(ControlCommand, std::span<const std::byte> = {}, std::chrono::milliseconds timeout = std::chrono::seconds(10));
    void stop() noexcept;
    Result<std::vector<HostNotification>> takeNotifications();
    uint32_t processId() const noexcept;
    std::string endpoint() const;
    Status openEditor(uint64_t parentWindow, float scale = 1.0f);
    void closeEditor() noexcept;
    Size editorSize() const noexcept;
    Status prepare(const ProcessConfig&);
    Status releaseAudio();
    void enqueueParameter(const ParameterEvent&) noexcept;
    Result<ParameterDisplay> parameterDisplay(uint32_t index);
    Result<std::vector<float>> parameterValues();
    Status flushParameters();
    Result<StateEnvelope> captureState(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    Status restoreState(const StateEnvelope&);
    std::optional<StateEnvelope> lastState() const;
    Result<ProgramInfo> programInfo();
    double tailLengthSeconds() const noexcept;
    ProcessResult process(AudioBlockView, EventBlockView, const TransportSnapshot&) noexcept;
    ProcessResult process(DoubleAudioBlockView, EventBlockView, const TransportSnapshot&) noexcept;
    uint32_t latencySamples() const noexcept;
    void resetProcessing() noexcept;
private:
    struct Impl;
    Status applyState(const StateEnvelope&, uint64_t sequenceBarrier, std::chrono::milliseconds timeout);
    std::unique_ptr<Impl> impl;
};
}
