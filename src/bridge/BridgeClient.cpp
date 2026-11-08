#include "BridgeClient.h"
#include "Control.h"
#include "common/Codec.h"
#include <bcrypt.h>
#include <mutex>
#include <atomic>
namespace nytera
{
struct BridgeClient::Impl
{
    std::timed_mutex mutex;
    std::unique_ptr<Control> control;
    Handle process, job;
    std::atomic<uint32_t> pid {0};
    std::atomic<uint32_t> editorWidth {480}, editorHeight {320};
    std::string pipeName;
    uint32_t sequence = 0;
    Result<Bytes> exchange(ControlCommand command, std::span<const std::byte> payload, std::chrono::milliseconds timeout)
    {
        if (!control) return Failure{ErrorCode::disconnected, "Helper is unavailable"};
        Frame frame {command, ++sequence, false, Bytes(payload.begin(), payload.end())};
        auto sent = control->send(frame, timeout);
        if (auto* failure = std::get_if<Failure>(&sent)) return *failure;
        auto response = control->receive(timeout);
        if (auto* failure = std::get_if<Failure>(&response)) return *failure;
        auto reply = std::get<Frame>(std::move(response));
        if (reply.command != command || reply.requestId != sequence) return Failure{ErrorCode::invalidData, "Bridge response ordering mismatch"};
        if (reply.failed)
        {
            Reader reader(reply.payload);
            auto code = static_cast<ErrorCode>(reader.number<uint32_t>());
            return Failure{code, reader.text()};
        }
        return std::move(reply.payload);
    }
};
BridgeClient::BridgeClient() : impl(std::make_unique<Impl>()) {}
BridgeClient::~BridgeClient() { stop(); }
Status BridgeClient::start(const Manifest& manifest, const std::filesystem::path& helper, const std::filesystem::path& source)
{
    stop();
    std::unique_lock lock(impl->mutex);
    try
    {
        Id128 random;
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(random.data()), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot generate instance ID");
        auto token = hex(random);
        impl->pipeName = "\\\\.\\pipe\\Nytera-" + token;
        auto server = Control::server(utf8Path(impl->pipeName).wstring());
        if (auto* failure = std::get_if<Failure>(&server)) return *failure;
        impl->control = std::make_unique<Control>(std::get<Control>(std::move(server)));
        impl->job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!impl->job.valid() || !SetInformationJobObject(impl->job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("Cannot create helper job");
        std::wstring command = quoteArgument(std::filesystem::absolute(helper).wstring()) + L" --serve "
            + quoteArgument(utf8Path(impl->pipeName).wstring()) + L" " + utf8Path(token).wstring();
        STARTUPINFOW startup {sizeof(STARTUPINFOW)};
        PROCESS_INFORMATION information {};
        if (!CreateProcessW(std::filesystem::absolute(helper).c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &information))
            throw std::runtime_error("Cannot launch helper " + std::to_string(GetLastError()));
        impl->process.reset(information.hProcess);
        Handle thread(information.hThread);
        impl->pid = information.dwProcessId;
        if (!AssignProcessToJobObject(impl->job.get(), impl->process.get()))
        {
            TerminateProcess(impl->process.get(), 1);
            throw std::runtime_error("Cannot attach helper job");
        }
        ResumeThread(thread.get());
        auto connected = impl->control->accept(std::chrono::seconds(60));
        if (auto* failure = std::get_if<Failure>(&connected)) throw std::runtime_error(failure->message);
        Writer handshake;
        handshake.text(token);
        handshake.text(pathUtf8(std::filesystem::absolute(source)));
        auto binding = encodeManifest(manifest);
        handshake.number(static_cast<uint32_t>(binding.size()));
        handshake.raw(binding);
        auto response = impl->exchange(ControlCommand::hello, handshake.bytes, std::chrono::seconds(60));
        if (auto* failure = std::get_if<Failure>(&response)) throw std::runtime_error(failure->message);
        return std::monostate{};
    }
    catch (const std::exception& error)
    {
        impl->control.reset();
        impl->job.reset();
        if (impl->process.valid()) WaitForSingleObject(impl->process.get(), 2000);
        impl->process.reset();
        impl->pid = 0;
        return Failure{ErrorCode::plugin, error.what()};
    }
}
Result<Bytes> BridgeClient::request(ControlCommand command, std::span<const std::byte> payload, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(impl->mutex, std::defer_lock);
    if (!lock.try_lock_for(timeout)) return Failure{ErrorCode::timeout, "Helper control channel is busy"};
    try { return impl->exchange(command, payload, timeout); }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
void BridgeClient::stop() noexcept
{
    try
    {
        std::unique_lock lock(impl->mutex);
        if (impl->control) impl->exchange(ControlCommand::shutdown, {}, std::chrono::seconds(2));
        impl->control.reset();
        if (impl->process.valid() && WaitForSingleObject(impl->process.get(), 2000) != WAIT_OBJECT_0)
            TerminateProcess(impl->process.get(), 1);
        impl->job.reset();
        impl->process.reset();
        impl->pid = 0;
    }
    catch (...) {}
}
std::vector<HostNotification> BridgeClient::takeNotifications()
{
    std::vector<HostNotification> notifications;
    auto reply = request(ControlCommand::poll);
    if (auto* data = std::get_if<Bytes>(&reply))
    {
        Reader reader(*data);
        auto count = reader.count(8192);
        for (uint32_t i = 0; i < count; ++i)
            notifications.push_back({reader.number<uint32_t>(), reader.number<int32_t>(), reader.number<int64_t>(), reader.real32()});
        reader.finish();
    }
    return notifications;
}
uint32_t BridgeClient::processId() const noexcept { return impl->pid.load(); }
std::string BridgeClient::endpoint() const { return impl->pipeName; }
Status BridgeClient::openEditor(uint64_t parentWindow, float scale)
{
    Writer writer;
    writer.number(parentWindow); writer.floating(scale);
    auto reply = request(ControlCommand::openEditor, writer.bytes);
    if (auto* failure = std::get_if<Failure>(&reply)) return *failure;
    try
    {
        Reader reader(std::get<Bytes>(reply));
        impl->editorWidth = reader.number<uint32_t>();
        impl->editorHeight = reader.number<uint32_t>();
        reader.finish();
        return std::monostate{};
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
void BridgeClient::closeEditor() noexcept
{
    try { request(ControlCommand::closeEditor, {}, std::chrono::seconds(2)); } catch (...) {}
}
Size BridgeClient::editorSize() const noexcept { return {impl->editorWidth.load(), impl->editorHeight.load()}; }
}
