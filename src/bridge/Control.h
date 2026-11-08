#pragma once
#include "common/Native.h"
#include "common/Wire.h"
#include <chrono>
#include <atomic>
#include <memory>
namespace nytera
{
class Control
{
public:
    Control() = default;
    static Result<Control> server(const std::wstring& endpoint);
    static Result<Control> connect(const std::wstring& endpoint, std::chrono::milliseconds);
    Status accept(std::chrono::milliseconds);
    Status send(const Frame&, std::chrono::milliseconds);
    Result<Frame> receive(std::chrono::milliseconds);
    Result<bool> available() const;
    void cancel() noexcept { if (cancelled) *cancelled = true; if (pipe.valid()) CancelIoEx(pipe.get(), nullptr); }
private:
    explicit Control(HANDLE handle) : pipe(handle) {}
    Status transfer(std::span<std::byte>, bool writing, std::chrono::steady_clock::time_point deadline);
    Handle pipe;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
};
}
