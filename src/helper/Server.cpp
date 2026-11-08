#include "Server.h"
#include "Vst2Instance.h"
#include "EditorHost.h"
#include "AudioWorker.h"
#include "bridge/Control.h"
#include "common/Codec.h"
#include "common/BoundedQueue.h"
#include <mutex>
#include <thread>
#include <atomic>

namespace nytera
{
class HelperServer
{
public:
    HelperServer(Control channel, std::string token) : channel(std::move(channel)), token(std::move(token))
    {
    }
    ~HelperServer()
    {
        audio.reset();
        if (instance) instance->onNotification = {};
        editor.reset();
        instance.reset();
    }
    int run()
    {
        while (running)
        {
            auto now = std::chrono::steady_clock::now();
            if (editor && now >= nextIdle)
            {
                editor->idle();
                nextIdle = now + std::chrono::milliseconds(20);
            }
            auto pending = pendingSize.exchange(0);
            if (editor && pending) editor->resize({static_cast<uint32_t>(pending >> 32), static_cast<uint32_t>(pending)});
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
    void postNotification(HostNotification event)
    {
        if (event.opcode == VST_HOST_OPCODE_EDITOR_RESIZE && event.index > 0 && event.value > 0)
            pendingSize = (uint64_t(static_cast<uint32_t>(event.index)) << 32) | static_cast<uint32_t>(event.value);
        if (event.opcode == VST_HOST_OPCODE_PARAM_START_EDIT || event.opcode == VST_HOST_OPCODE_PARAM_STOP_EDIT)
        {
            if (event.index < 0 || static_cast<size_t>(event.index) >= actual.parameters.size()) return;
            auto& active = activeGestures[event.index];
            if (event.opcode == VST_HOST_OPCODE_PARAM_START_EDIT)
            {
                uint64_t id = ++gestureSequence, empty = 0;
                if (!active.compare_exchange_strong(empty, id)) return;
                event.value = static_cast<int64_t>(id);
            }
            else
            {
                event.value = static_cast<int64_t>(active.exchange(0));
                if (!event.value) return;
            }
        }
        if (!notifications->push(event)) notificationOverflow = true;
    }
    void closeGestures()
    {
        for (uint32_t index = 0; index < actual.parameters.size(); ++index)
        {
            auto id = activeGestures[index].exchange(0);
            if (id && !notifications->push({44, static_cast<int32_t>(index), static_cast<int64_t>(id), 0})) notificationOverflow = true;
        }
    }
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
                    || actual.inputs.size() != expected.inputs.size() || actual.outputs.size() != expected.outputs.size()
                    || (expected.schemaVersion >= 2 && actual.programs.names.size() != expected.programs.names.size()))
                    throw Failure{ErrorCode::identity, "Source plugin does not match embedded binding"};
                activeGestures = std::make_unique<std::atomic<uint64_t>[]>(actual.parameters.size());
                closedGestures.resize(actual.parameters.size());
                publishedGestures.resize(actual.parameters.size());
                instance->onNotification = [this](const HostNotification& event) { postNotification(event); };
                response.payload = encodeManifest(actual);
                return response;
            }
            if (request.command == ControlCommand::shutdown) { running = false; return response; }
            if (!instance) throw Failure{ErrorCode::plugin, "Helper has no source instance"};
            switch (request.command)
            {
                case ControlCommand::inspect: response.payload = encodeManifest(actual); break;
                case ControlCommand::openEditor:
                {
                    Reader reader(request.payload);
                    auto parent = reader.number<uint64_t>();
                    reader.real32();
                    reader.finish();
                    if (parent > UINTPTR_MAX) throw Failure{ErrorCode::invalidData, "Host window handle is out of range"};
                    editor = std::make_unique<EditorHost>(*instance);
                    std::lock_guard lock(sourceMutex);
                    auto opened = editor->open(reinterpret_cast<HWND>(static_cast<uintptr_t>(parent)));
                    if (auto* failure = std::get_if<Failure>(&opened)) throw *failure;
                    Writer writer;
                    writer.number(editor->size().width); writer.number(editor->size().height);
                    response.payload = std::move(writer.bytes);
                    break;
                }
                case ControlCommand::closeEditor: editor.reset(); closeGestures(); break;
                case ControlCommand::releaseAudio:
                    audio.reset();
                    instance->suspend();
                    break;
                case ControlCommand::connectAudio:
                {
                    Reader reader(request.payload);
                    auto name = utf8Path(reader.text()).wstring();
                    ProcessConfig config;
                    config.parameterCount = static_cast<uint32_t>(actual.parameters.size());
                    config.programCount = static_cast<uint32_t>(expected.programs.names.size());
                    config.sampleRate = reader.real64(); config.maximumBlock = reader.number<uint32_t>();
                    config.inputs = reader.number<uint32_t>(); config.outputs = reader.number<uint32_t>(); config.offline = reader.boolean();
                    reader.finish();
                    if (config.inputs != actual.inputs.size() || config.outputs != actual.outputs.size())
                        throw Failure{ErrorCode::invalidData, "Requested channels differ from source topology"};
                    audio.reset();
                    auto saved = instance->captureState();
                    if (auto* failure = std::get_if<Failure>(&saved)) throw *failure;
                    auto mapped = SharedAudio::open(name, config);
                    if (auto* failure = std::get_if<Failure>(&mapped)) throw *failure;
                    auto memory = std::get<std::unique_ptr<SharedAudio>>(std::move(mapped));
                    auto sourceConfig = config;
                    sourceConfig.maximumBlock = memory->quantum;
                    auto prepared = instance->prepare(sourceConfig);
                    if (auto* failure = std::get_if<Failure>(&prepared)) throw *failure;
                    auto restored = instance->restoreState(std::get<StateEnvelope>(saved));
                    if (auto* failure = std::get_if<Failure>(&restored)) throw *failure;
                    audio = std::make_unique<AudioWorker>(std::move(memory), *instance, sourceMutex);
                    Writer writer;
                    writer.number(static_cast<uint32_t>(std::max(0, instance->native()->delay)));
                    writer.floating(instance->tailSeconds());
                    response.payload = std::move(writer.bytes);
                    break;
                }
                case ControlCommand::poll:
                {
                    if (notificationOverflow) throw Failure{ErrorCode::plugin, "Source notification queue overflow"};
                    std::lock_guard lock(sourceMutex);
                    auto* effect = instance->native();
                    if (effect->num_inputs != static_cast<int32_t>(actual.inputs.size()) || effect->num_outputs != static_cast<int32_t>(actual.outputs.size())
                        || effect->num_params != static_cast<int32_t>(actual.parameters.size())
                        || std::max(1, effect->num_programs) != static_cast<int32_t>(actual.programs.names.size()))
                    {
                        if (audio) audio->fail();
                        throw Failure{ErrorCode::unsupported, "Source topology changed. Reopen the same conversion to retry."};
                    }
                    std::vector<HostNotification> pending;
                    HostNotification notification;
                    uint32_t drained = 0;
                    while (drained++ < 8192 && notifications->pop(notification))
                    {
                        if (notification.opcode == 43 || notification.opcode == 44)
                        {
                            auto index = static_cast<uint32_t>(notification.index);
                            if (index >= closedGestures.size()) continue;
                            auto id = static_cast<uint64_t>(notification.value);
                            if (notification.opcode == 43)
                            {
                                if (id <= closedGestures[index]) continue;
                                publishedGestures[index] = id;
                            }
                            else
                            {
                                closedGestures[index] = std::max(closedGestures[index], id);
                                if (publishedGestures[index] != id) continue;
                                publishedGestures[index] = 0;
                            }
                        }
                        if (notification.opcode == VST_HOST_OPCODE_IO_MODIFIED)
                        {
                            auto refreshed = instance->refreshLatency();
                            if (auto* failure = std::get_if<Failure>(&refreshed)) throw *failure;
                            notification.value = std::max(0, effect->delay);
                            notification.normalized = static_cast<float>(instance->tailSeconds());
                        }
                        pending.push_back(notification);
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
                case ControlCommand::parameter:
                {
                    Reader reader(request.payload);
                    auto count = reader.count(8192);
                    std::vector<ParameterEvent> parameters;
                    for (uint32_t i = 0; i < count; ++i)
                        parameters.push_back({reader.number<uint32_t>(), 0, reader.real32(), reader.number<uint32_t>(), reader.number<uint64_t>()});
                    reader.finish();
                    std::lock_guard lock(sourceMutex);
                    for (const auto& event : parameters)
                    {
                        auto applied = instance->applyParameter(event);
                        if (auto* failure = std::get_if<Failure>(&applied)) throw *failure;
                        if (audio) audio->acknowledge(event);
                    }
                    Writer writer;
                    writer.number(static_cast<uint32_t>(actual.parameters.size()));
                    for (uint32_t i = 0; i < actual.parameters.size(); ++i)
                    {
                        writer.floating(instance->native()->get_parameter(instance->native(), i));
                        writer.number(instance->parameterSequence(i));
                    }
                    response.payload = std::move(writer.bytes);
                    break;
                }
                case ControlCommand::display:
                {
                    Reader reader(request.payload);
                    auto index = reader.number<uint32_t>(); reader.finish();
                    std::lock_guard lock(sourceMutex);
                    auto text = instance->parameterDisplay(index);
                    if (auto* failure = std::get_if<Failure>(&text)) throw *failure;
                    Writer writer;
                    writer.floating(instance->native()->get_parameter(instance->native(), index));
                    writer.text(std::get<std::string>(text)); response.payload = std::move(writer.bytes);
                    break;
                }
                case ControlCommand::programs:
                {
                    std::lock_guard lock(sourceMutex);
                    auto programs = instance->programInfo();
                    if (auto* failure = std::get_if<Failure>(&programs)) throw *failure;
                    response.payload = encodePrograms(std::get<ProgramInfo>(programs));
                    break;
                }
                case ControlCommand::captureState:
                {
                    Reader reader(request.payload);
                    auto count = reader.count(73729);
                    std::vector<ParameterEvent> parameters;
                    for (uint32_t i = 0; i < count; ++i)
                        parameters.push_back({reader.number<uint32_t>(), 0, reader.real32(), reader.number<uint32_t>(), reader.number<uint64_t>()});
                    reader.finish();
                    std::sort(parameters.begin(), parameters.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
                    std::lock_guard lock(sourceMutex);
                    for (const auto& parameter : parameters)
                    {
                        auto applied = instance->applyParameter(parameter);
                        if (auto* failure = std::get_if<Failure>(&applied)) throw *failure;
                        if (audio) audio->acknowledge(parameter);
                    }
                    auto snapshot = instance->captureState();
                    if (auto* failure = std::get_if<Failure>(&snapshot)) throw *failure;
                    response.payload = encodeState(std::get<StateEnvelope>(snapshot));
                    break;
                }
                case ControlCommand::restoreState:
                {
                    Reader reader(request.payload);
                    auto barrier = reader.number<uint64_t>();
                    auto state = decodeState(reader.raw(reader.count(static_cast<uint32_t>(maximumStateBytes))));
                    reader.finish();
                    if (auto* failure = std::get_if<Failure>(&state)) throw *failure;
                    std::lock_guard lock(sourceMutex);
                    auto restored = instance->restoreState(std::get<StateEnvelope>(state), barrier);
                    if (auto* failure = std::get_if<Failure>(&restored)) throw *failure;
                    if (audio) audio->acknowledgeState(barrier);
                    HostNotification old;
                    for (uint32_t i = 0; i < 8192 && notifications->pop(old); ++i) {}
                    closeGestures();
                    auto programs = instance->programInfo();
                    if (auto* failure = std::get_if<Failure>(&programs)) throw *failure;
                    auto data = encodePrograms(std::get<ProgramInfo>(programs));
                    Writer writer;
                    writer.number(static_cast<uint32_t>(data.size())); writer.raw(data);
                    writer.number(static_cast<uint32_t>(actual.parameters.size()));
                    for (uint32_t i = 0; i < actual.parameters.size(); ++i) writer.floating(instance->native()->get_parameter(instance->native(), i));
                    writer.number(static_cast<uint32_t>(std::max(0, instance->native()->delay))); writer.floating(instance->tailSeconds());
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
    std::unique_ptr<EditorHost> editor;
    std::unique_ptr<AudioWorker> audio;
    std::mutex sourceMutex;
    Manifest expected, actual;
    bool running = true;
    std::unique_ptr<BoundedQueue<HostNotification, 8192>> notifications = std::make_unique<BoundedQueue<HostNotification, 8192>>();
    std::atomic<bool> notificationOverflow {false};
    std::unique_ptr<std::atomic<uint64_t>[]> activeGestures;
    std::vector<uint64_t> closedGestures, publishedGestures;
    std::atomic<uint64_t> gestureSequence {0};
    std::atomic<uint64_t> pendingSize {0};
    std::chrono::steady_clock::time_point nextIdle {};
};

int runHelperServer(const std::wstring& endpoint, const std::string& token)
{
    auto connected = Control::connect(endpoint, std::chrono::seconds(60));
    if (std::holds_alternative<Failure>(connected)) return 1;
    HelperServer server(std::get<Control>(std::move(connected)), token);
    return server.run();
}
}
