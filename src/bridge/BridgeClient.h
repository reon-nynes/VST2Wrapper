#pragma once
#include "common/Manifest.h"
#include "common/Processing.h"
#include "common/Wire.h"
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
    std::vector<HostNotification> takeNotifications();
    uint32_t processId() const noexcept;
    std::string endpoint() const;
    Status openEditor(uint64_t parentWindow, float scale = 1.0f);
    void closeEditor() noexcept;
    Size editorSize() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
