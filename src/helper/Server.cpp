#include "Server.h"
#include "Vst2Instance.h"
#include "bridge/Control.h"
#include "common/Codec.h"
#include <mutex>
#include <thread>

namespace nytera
{
class HelperServer
{
public:
    HelperServer(Control channel, std::string token) : channel(std::move(channel)), token(std::move(token))
    {
        notifications.reserve(8192);
    }
    int run()
    {
        while (running)
        {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT) return 0;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            auto available = channel.available();
            if (std::holds_alternative<Failure>(available)) return 1;
            if (!std::get<bool>(available)) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            auto incoming = channel.receive(std::chrono::seconds(10));
            if (std::holds_alternative<Failure>(incoming)) return 1;
            auto response = handle(std::get<Frame>(std::move(incoming)));
            auto sent = channel.send(response, std::chrono::seconds(10));
            if (std::holds_alternative<Failure>(sent)) return 1;
        }
        return 0;
    }
private:
    Frame handle(Frame request)
    {
        Frame response {request.command, request.requestId, false, {}};
        try
        {
            if (request.command == ControlCommand::hello)
            {
                if (instance) throw Failure{ErrorCode::invalidData, "Duplicate startup request"};
                Reader reader(request.payload);
                if (reader.text() != token) throw Failure{ErrorCode::invalidData, "Invalid helper launch token"};
                auto source = utf8Path(reader.text());
                auto binding = decodeManifest(reader.raw(reader.count(16 * 1024 * 1024)));
                reader.finish();
                if (auto* failure = std::get_if<Failure>(&binding)) throw *failure;
                expected = std::get<Manifest>(std::move(binding));
                std::filesystem::current_path(source.parent_path());
                auto loaded = Vst2Instance::open(source);
                if (auto* failure = std::get_if<Failure>(&loaded)) throw *failure;
                instance = std::get<std::unique_ptr<Vst2Instance>>(std::move(loaded));
                auto inspected = instance->inspect();
                if (auto* failure = std::get_if<Failure>(&inspected)) throw *failure;
                actual = std::get<Manifest>(std::move(inspected));
                if (actual.sourceHash != expected.sourceHash || actual.sourceUniqueId != expected.sourceUniqueId
                    || actual.architecture != expected.architecture || actual.parameters.size() != expected.parameters.size()
                    || actual.inputs.size() != expected.inputs.size() || actual.outputs.size() != expected.outputs.size())
                    throw Failure{ErrorCode::identity, "Source plugin does not match embedded binding"};
                instance->onNotification = [this](const HostNotification& event)
                {
                    std::lock_guard lock(notificationMutex);
                    if (notifications.size() < 8192) notifications.push_back(event);
                };
                response.payload = encodeManifest(actual);
                return response;
            }
            if (request.command == ControlCommand::shutdown) { running = false; return response; }
            if (!instance) throw Failure{ErrorCode::plugin, "Helper has no source instance"};
            switch (request.command)
            {
                case ControlCommand::inspect: response.payload = encodeManifest(actual); break;
                case ControlCommand::poll:
                {
                    std::vector<HostNotification> pending;
                    {
                        std::lock_guard lock(notificationMutex);
                        pending.swap(notifications);
                        notifications.reserve(8192);
                    }
                    Writer writer;
                    writer.number(static_cast<uint32_t>(pending.size()));
                    for (const auto& event : pending)
                    {
                        writer.number(event.opcode); writer.number(event.index); writer.number(event.value); writer.floating(event.normalized);
                    }
                    response.payload = std::move(writer.bytes);
                    break;
                }
                default: throw Failure{ErrorCode::unsupported, "Unsupported helper command"};
            }
        }
        catch (const Failure& failure)
        {
            response.failed = true;
            Writer writer;
            writer.number(static_cast<uint32_t>(failure.code)); writer.text(failure.message);
            response.payload = std::move(writer.bytes);
        }
        catch (const std::exception& error)
        {
            response.failed = true;
            Writer writer;
            writer.number(static_cast<uint32_t>(ErrorCode::invalidData)); writer.text(error.what());
            response.payload = std::move(writer.bytes);
        }
        return response;
    }
    Control channel;
    std::string token;
    std::unique_ptr<Vst2Instance> instance;
    Manifest expected, actual;
    bool running = true;
    std::mutex notificationMutex;
    std::vector<HostNotification> notifications;
};

int runHelperServer(const std::wstring& endpoint, const std::string& token)
{
    auto connected = Control::connect(endpoint, std::chrono::seconds(60));
    if (std::holds_alternative<Failure>(connected)) return 1;
    HelperServer server(std::get<Control>(std::move(connected)), token);
    return server.run();
}
}
