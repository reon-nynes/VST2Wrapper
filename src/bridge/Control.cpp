#include "Control.h"
#include <sddl.h>
#include <algorithm>
#include <thread>
namespace nytera
{
static DWORD remainingTime(std::chrono::steady_clock::time_point deadline)
{
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return static_cast<DWORD>(std::clamp<int64_t>(milliseconds, 0, MAXDWORD - 1));
}
static bool waitForIo(HANDLE event, std::chrono::steady_clock::time_point deadline)
{
    // Creating a child HWND in another process can synchronously notify its
    // parent. Dispatch sent messages while waiting, without running arbitrary
    // posted UI commands inside a control request.
    for (;;)
    {
        auto result = MsgWaitForMultipleObjectsEx(1, &event, remainingTime(deadline), QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0) return true;
        if (result != WAIT_OBJECT_0 + 1 || std::chrono::steady_clock::now() >= deadline) return false;
        MSG message;
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    }
}
Result<Control> Control::server(const std::wstring& endpoint)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
        return Failure{ErrorCode::io, "Cannot create pipe security descriptor"};
    SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    HANDLE pipe = CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &attributes);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) return Failure{ErrorCode::io, "Cannot create bridge pipe " + std::to_string(GetLastError())};
    return Control(pipe);
}
Result<Control> Control::connect(const std::wstring& endpoint, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    do
    {
        HANDLE pipe = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return Control(pipe);
        auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return Failure{ErrorCode::io, "Cannot connect bridge pipe " + std::to_string(error)};
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return Failure{ErrorCode::timeout, "Bridge connection timed out"};
}
Status Control::accept(std::chrono::milliseconds timeout)
{
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED operation {};
    operation.hEvent = event.get();
    if (ConnectNamedPipe(pipe.get(), &operation)) return std::monostate{};
    auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return std::monostate{};
    if (error != ERROR_IO_PENDING) return Failure{ErrorCode::io, "Bridge accept failed"};
    if (!waitForIo(event.get(), std::chrono::steady_clock::now() + timeout))
    {
        CancelIoEx(pipe.get(), &operation);
        DWORD ignored;
        GetOverlappedResult(pipe.get(), &operation, &ignored, TRUE);
        return Failure{ErrorCode::timeout, "Helper did not connect"};
    }
    DWORD ignored;
    if (!GetOverlappedResult(pipe.get(), &operation, &ignored, FALSE)) return Failure{ErrorCode::disconnected, "Helper connection closed"};
    return std::monostate{};
}
Status Control::transfer(std::span<std::byte> bytes, bool writing, std::chrono::steady_clock::time_point deadline)
{
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return Failure{ErrorCode::io, "Cannot create IO event"};
    while (!bytes.empty())
    {
        OVERLAPPED operation {};
        operation.hEvent = event.get();
        ResetEvent(event.get());
        DWORD transferred = 0;
        auto amount = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1024 * 1024));
        BOOL success = writing ? WriteFile(pipe.get(), bytes.data(), amount, &transferred, &operation)
                               : ReadFile(pipe.get(), bytes.data(), amount, &transferred, &operation);
        if (!success)
        {
            if (GetLastError() != ERROR_IO_PENDING) return Failure{ErrorCode::disconnected, "Helper pipe disconnected"};
            if (!waitForIo(event.get(), deadline))
            {
                CancelIoEx(pipe.get(), &operation);
                GetOverlappedResult(pipe.get(), &operation, &transferred, TRUE);
                return Failure{ErrorCode::timeout, "Helper request timed out"};
            }
            if (!GetOverlappedResult(pipe.get(), &operation, &transferred, FALSE))
                return Failure{ErrorCode::disconnected, "Helper IO failed"};
        }
        if (transferred == 0) return Failure{ErrorCode::disconnected, "Helper pipe closed"};
        bytes = bytes.subspan(transferred);
    }
    return std::monostate{};
}
Status Control::send(const Frame& frame, std::chrono::milliseconds timeout)
{
    auto data = encodeFrame(frame);
    return transfer(data, true, std::chrono::steady_clock::now() + timeout);
}
Result<Frame> Control::receive(std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    Bytes bytes(24);
    auto header = transfer(bytes, false, deadline);
    if (auto* failure = std::get_if<Failure>(&header)) return *failure;
    auto size = framePayloadSize(bytes);
    if (auto* failure = std::get_if<Failure>(&size)) return *failure;
    bytes.resize(24 + std::get<uint32_t>(size));
    auto payload = transfer(std::span(bytes).subspan(24), false, deadline);
    if (auto* failure = std::get_if<Failure>(&payload)) return *failure;
    return decodeFrame(bytes);
}
Result<bool> Control::available() const
{
    DWORD available = 0;
    if (!PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr))
        return Failure{ErrorCode::disconnected, "Helper pipe closed"};
    return available != 0;
}
}
