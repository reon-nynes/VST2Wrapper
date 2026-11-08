#include "BridgeClient.h"
#include "Control.h"
#include "AudioTransport.h"
#include "common/Codec.h"
#include "common/Deadline.h"
#include "common/BoundedQueue.h"
#include <bcrypt.h>
#include <mutex>
#include <atomic>
#include <cmath>
namespace nytera
{
struct BridgeClient::Impl
{
    struct Session
    {
        std::timed_mutex mutex;
        std::unique_ptr<Control> control;
        Handle process, job;
        uint32_t sequence = 0;
    };
    std::atomic<std::shared_ptr<Session>> session;
    std::recursive_mutex lifecycle;
    std::atomic<uint64_t> stopEpoch {0};
    std::atomic<BridgePhase> phase {BridgePhase::stopped};
    std::mutex diagnosticMutex;
    std::string diagnostic;
    std::atomic<bool> renderFailed {false}, notesOff {false};
    uint32_t consecutiveUnderruns = 0;
    std::filesystem::path helperPath, sourcePath;
    std::optional<ProcessConfig> preparedConfig;
    uint64_t editorParent = 0;
    float editorScale = 1;
    void fail(const Failure& failure, const std::shared_ptr<Session>& current)
    {
        if (session.load() != current || !current) return;
        ready = false;
        notesOff = true;
        phase = BridgePhase::failed;
        { std::lock_guard lock(diagnosticMutex); diagnostic = failure.message; }
        current->control->cancel();
    }
    std::atomic<uint32_t> pid {0};
    std::atomic<uint32_t> editorWidth {480}, editorHeight {320};
    std::atomic<uint32_t> totalLatency {0};
    std::atomic<uint32_t> pipelineLatency {0};
    std::atomic<double> tail {0};
    std::atomic<bool> ready {false}, maintenance {false};
    std::atomic<uint32_t> activeCallbacks {0};
    Handle stopEvent {CreateEventW(nullptr, TRUE, TRUE, nullptr)};
    std::atomic<uint64_t> skippedSamples {0};
    std::timed_mutex stateOperations;
    std::mutex snapshots;
    std::optional<Manifest> binding;
    std::optional<StateEnvelope> completeState, pendingState;
    uint64_t pendingBarrier = 0, stateBarrier = 0, completeBarrier = 0;
    struct Resume
    {
        Impl& owner;
        ~Resume() { owner.maintenance = false; }
    };
    bool pause(std::chrono::milliseconds timeout)
    {
        maintenance = true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (activeCallbacks.load() != 0)
        {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            Sleep(1);
        }
        return true;
    }
    template<class Block> ProcessResult render(Block block, EventBlockView events, const TransportSnapshot& transport) noexcept
    {
        auto silence = [&](ProcessResult result)
        {
            for (auto* output : block.outputs) if (output) std::fill_n(output, block.samples, 0);
            return result;
        };
        if (!ready.load())
        {
            if (notesOff.exchange(false) && events.emit)
                for (uint8_t channel = 0; channel < 16; ++channel)
                {
                    std::array<std::byte, 3> off {static_cast<std::byte>(0xb0 | channel), std::byte{123}, std::byte{0}};
                    events.emit(events.context, transport.streamSamples, off);
                }
            return silence(ProcessResult::failed);
        }
        if (maintenance.load()) { skippedSamples.fetch_add(block.samples); return silence(ProcessResult::priming); }
        activeCallbacks.fetch_add(1);
        struct Finish { std::atomic<uint32_t>& count; ~Finish() { count.fetch_sub(1); } } finish {activeCallbacks};
        if (maintenance.load()) { skippedSamples.fetch_add(block.samples); return silence(ProcessResult::priming); }
        if (!audio) return silence(ProcessResult::failed);
        auto skipped = skippedSamples.exchange(0);
        if (skipped) audio->skipSamples(skipped);
        collectParameters(events);
        auto result = audio->process(block, events, transport);
        if (result == ProcessResult::underrun) ++consecutiveUnderruns;
        else consecutiveUnderruns = 0;
        if (result == ProcessResult::failed || consecutiveUnderruns >= 8) renderFailed = true;
        return result;
    }
    std::unique_ptr<AudioTransport> audio;
    uint32_t audioGeneration = 0;
    std::unique_ptr<BoundedQueue<ParameterEvent, 8192>> parameterQueue = std::make_unique<BoundedQueue<ParameterEvent, 8192>>();
    std::vector<ParameterEvent> parameterScratch;
    std::atomic<bool> parameterOverflow {false};
    uint32_t parameterCount = 0;
    std::atomic<uint64_t> parameterSequence {0};
    void collectParameters(EventBlockView& events)
    {
        parameterScratch.clear();
        for (auto event : events.parameters)
        {
            if (parameterScratch.size() == 8192) { parameterOverflow = true; break; }
            if (event.sequence == 0) event.sequence = ++parameterSequence;
            parameterScratch.push_back(event);
        }
        ParameterEvent event;
        while (parameterScratch.size() < 8192 && parameterQueue->pop(event)) parameterScratch.push_back(event);
        events.parameters = parameterScratch;
        if (parameterOverflow.load() && audio) InterlockedExchange(&audio->shared().header().error, 1);
    }
    std::string pipeName;
    Result<Bytes> exchange(const std::shared_ptr<Session>& current, ControlCommand command, std::span<const std::byte> payload, std::chrono::milliseconds timeout)
    {
        if (!current) return Failure{ErrorCode::disconnected, "Helper is unavailable"};
        Deadline deadline(timeout);
        Frame frame {command, ++current->sequence, false, Bytes(payload.begin(), payload.end())};
        auto sent = current->control->send(frame, deadline.remaining());
        if (auto* failure = std::get_if<Failure>(&sent)) return *failure;
        auto response = current->control->receive(deadline.remaining());
        if (auto* failure = std::get_if<Failure>(&response)) return *failure;
        auto reply = std::get<Frame>(std::move(response));
        if (reply.command != command || reply.requestId != current->sequence) return Failure{ErrorCode::invalidData, "Bridge response ordering mismatch"};
        if (reply.failed)
        {
            Reader reader(reply.payload);
            auto code = static_cast<ErrorCode>(reader.number<uint32_t>());
            return Failure{code, reader.text()};
        }
        return std::move(reply.payload);
    }
};
BridgeClient::BridgeClient() : impl(std::make_unique<Impl>()) { impl->parameterScratch.reserve(8192); }
BridgeClient::~BridgeClient() { stop(); }
BridgeStatus BridgeClient::status() const noexcept
{
    try
    {
        auto current = impl->session.load();
        if (current && impl->phase == BridgePhase::ready)
        {
            if (WaitForSingleObject(current->process.get(), 0) == WAIT_OBJECT_0)
                impl->fail({ErrorCode::disconnected, "Source helper exited. Retry to restore the saved instance."}, current);
            else if (impl->renderFailed.exchange(false))
                impl->fail({ErrorCode::plugin, "Source processing failed or repeatedly missed its deadline. Retry the instance."}, current);
        }
        std::lock_guard lock(impl->diagnosticMutex);
        return {impl->phase.load(), impl->diagnostic};
    }
    catch (...) { return {BridgePhase::failed, {}}; }
}
Status BridgeClient::retry()
{
    std::lock_guard lifecycle(impl->lifecycle);
    std::optional<Manifest> binding;
    { std::lock_guard lock(impl->snapshots); binding = impl->binding; }
    if (!binding) return Failure{ErrorCode::identity, "This instance has no bound source"};
    auto saved = lastState();
    auto config = impl->preparedConfig;
    auto parent = impl->editorParent; auto scale = impl->editorScale;
    stop();
    if (saved) { auto restored = restoreState(*saved); if (auto* failure = std::get_if<Failure>(&restored)) return *failure; }
    auto started = start(*binding, impl->helperPath, impl->sourcePath);
    if (auto* failure = std::get_if<Failure>(&started)) return *failure;
    if (config) { auto prepared = prepare(*config); if (auto* failure = std::get_if<Failure>(&prepared)) return *failure; }
    if (parent && IsWindow(reinterpret_cast<HWND>(parent))) return openEditor(parent, scale);
    return std::monostate{};
}
Status BridgeClient::start(const Manifest& manifest, const std::filesystem::path& helper, const std::filesystem::path& source, std::function<bool()> cancelled)
{
    std::lock_guard lifecycle(impl->lifecycle);
    {
        std::lock_guard snapshotLock(impl->snapshots);
        if (impl->binding && (impl->binding->conversionId != manifest.conversionId || impl->binding->sourceHash != manifest.sourceHash))
            return Failure{ErrorCode::identity, "An instance cannot switch its bound source DLL"};
        if (impl->pendingState)
        {
            auto valid = validateStateBinding(*impl->pendingState, manifest);
            if (auto* failure = std::get_if<Failure>(&valid)) return *failure;
        }
        impl->binding = manifest;
    }
    stop();
    auto epoch = impl->stopEpoch.load();
    Deadline deadline(std::chrono::seconds(60));
    std::unique_lock stateOperation(impl->stateOperations, std::defer_lock);
    if (!stateOperation.try_lock_for(deadline.remaining())) return Failure{ErrorCode::timeout, "Previous source operation did not stop"};
    auto current = std::make_shared<Impl::Session>();
    std::unique_lock lock(current->mutex);
    impl->phase = BridgePhase::starting;
    impl->helperPath = helper; impl->sourcePath = source;
    impl->renderFailed = false; impl->parameterOverflow = false;
    impl->consecutiveUnderruns = 0;
    { std::lock_guard diagnosticLock(impl->diagnosticMutex); impl->diagnostic.clear(); }
    try
    {
        if (!impl->stopEvent.valid()) throw std::runtime_error("Cannot create source cancellation event");
        if (!impl->pause(deadline.remaining())) throw std::runtime_error("Previous audio callback did not stop");
        impl->maintenance = false;
        ResetEvent(impl->stopEvent.get());
        impl->totalLatency = manifest.initialDelay;
        impl->pipelineLatency = 0;
        impl->tail = manifest.tailSeconds;
        impl->parameterCount = static_cast<uint32_t>(manifest.parameters.size());
        Id128 random;
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(random.data()), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot generate instance ID");
        auto token = hex(random);
        impl->pipeName = "\\\\.\\pipe\\Nytera-" + token;
        auto server = Control::server(utf8Path(impl->pipeName).wstring());
        if (auto* failure = std::get_if<Failure>(&server)) throw std::runtime_error(failure->message);
        current->control = std::make_unique<Control>(std::get<Control>(std::move(server)));
        current->job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!current->job.valid() || !SetInformationJobObject(current->job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("Cannot create helper job");
        std::wstring command = quoteArgument(std::filesystem::absolute(helper).wstring()) + L" --serve "
            + quoteArgument(utf8Path(impl->pipeName).wstring()) + L" " + utf8Path(token).wstring();
        STARTUPINFOW startup {sizeof(STARTUPINFOW)};
        PROCESS_INFORMATION information {};
        if (!CreateProcessW(std::filesystem::absolute(helper).c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &information))
            throw std::runtime_error("Cannot launch helper " + std::to_string(GetLastError()));
        current->process.reset(information.hProcess);
        Handle thread(information.hThread);
        impl->pid = information.dwProcessId;
        if (!AssignProcessToJobObject(current->job.get(), current->process.get()))
        {
            TerminateProcess(current->process.get(), 1);
            throw std::runtime_error("Cannot attach helper job");
        }
        impl->session = current;
        if (epoch != impl->stopEpoch.load() || (cancelled && cancelled())) throw std::runtime_error("Helper startup cancelled");
        ResumeThread(thread.get());
        auto connected = current->control->accept(deadline.remaining());
        if (auto* failure = std::get_if<Failure>(&connected)) throw std::runtime_error(failure->message);
        Writer handshake;
        handshake.text(token);
        handshake.text(pathUtf8(std::filesystem::absolute(source)));
        auto binding = encodeManifest(manifest);
        handshake.number(static_cast<uint32_t>(binding.size()));
        handshake.raw(binding);
        auto response = impl->exchange(current, ControlCommand::hello, handshake.bytes, deadline.remaining());
        if (auto* failure = std::get_if<Failure>(&response)) throw std::runtime_error(failure->message);
        lock.unlock();
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            std::optional<StateEnvelope> pending;
            uint64_t barrier = 0;
            {
                std::lock_guard snapshotLock(impl->snapshots);
                pending = impl->pendingState; barrier = impl->pendingBarrier;
                if (!pending)
                {
                    if (impl->session.load() != current) throw std::runtime_error("Helper startup cancelled");
                    impl->ready = true; impl->phase = BridgePhase::ready; return std::monostate{};
                }
            }
            auto restored = applyState(*pending, barrier, deadline.remaining());
            if (auto* failure = std::get_if<Failure>(&restored)) throw std::runtime_error(failure->message);
            {
                std::lock_guard snapshotLock(impl->snapshots);
                if (impl->pendingBarrier == barrier) impl->pendingState.reset();
            }
        }
        throw std::runtime_error("State requests did not settle during startup");
    }
    catch (const std::exception& error)
    {
        Failure failure {ErrorCode::plugin, error.what()};
        impl->fail(failure, current);
        if (epoch == impl->stopEpoch.load())
        {
            impl->phase = BridgePhase::failed;
            std::lock_guard diagnosticLock(impl->diagnosticMutex); impl->diagnostic = failure.message;
        }
        if (current->job.valid()) TerminateJobObject(current->job.get(), 1);
        impl->pid = 0;
        return failure;
    }
}
Result<Bytes> BridgeClient::request(ControlCommand command, std::span<const std::byte> payload, std::chrono::milliseconds timeout)
{
    Deadline deadline(timeout);
    auto current = impl->session.load();
    if (!current) return Failure{ErrorCode::disconnected, "Helper is unavailable"};
    std::unique_lock lock(current->mutex, std::defer_lock);
    if (!lock.try_lock_for(deadline.remaining())) return Failure{ErrorCode::timeout, "Helper control channel is busy"};
    try
    {
        auto result = impl->exchange(current, command, payload, deadline.remaining());
        if (auto* failure = std::get_if<Failure>(&result)) impl->fail(*failure, current);
        return result;
    }
    catch (const std::exception& error)
    {
        Failure failure {ErrorCode::invalidData, error.what()};
        impl->fail(failure, current); return failure;
    }
}
void BridgeClient::stop() noexcept
{
    ++impl->stopEpoch;
    impl->ready = false;
    if (impl->stopEvent.valid()) SetEvent(impl->stopEvent.get());
    impl->notesOff = true;
    impl->phase = BridgePhase::stopped;
    impl->pid = 0;
    try
    {
        auto current = impl->session.exchange(nullptr);
        if (!current) return;
        std::unique_lock lock(current->mutex, std::defer_lock);
        if (lock.try_lock_for(std::chrono::milliseconds(5)))
            impl->exchange(current, ControlCommand::shutdown, {}, std::chrono::milliseconds(100));
        current->control->cancel();
        if (current->job.valid()) TerminateJobObject(current->job.get(), 0);
        if (current->process.valid()) WaitForSingleObject(current->process.get(), 300);
    }
    catch (...) {}
}

Result<std::vector<HostNotification>> BridgeClient::takeNotifications()
{
    try
    {
        auto reply = request(ControlCommand::poll, {}, std::chrono::seconds(2));
        if (auto* failure = std::get_if<Failure>(&reply)) return *failure;
        Reader reader(std::get<Bytes>(reply));
        std::vector<HostNotification> notifications;
        auto count = reader.count(8192);
        for (uint32_t i = 0; i < count; ++i)
        {
            HostNotification event {reader.number<uint32_t>(), reader.number<int32_t>(), reader.number<int64_t>(), reader.real32()};
            if (event.opcode == 15 && event.index > 0 && event.value > 0)
            { impl->editorWidth = static_cast<uint32_t>(event.index); impl->editorHeight = static_cast<uint32_t>(event.value); }
            if (event.opcode == 13 && event.value >= 0 && event.value <= INT32_MAX)
                impl->totalLatency = impl->pipelineLatency.load() + static_cast<uint32_t>(event.value);
            notifications.push_back(event);
        }
        reader.finish();
        return notifications;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
uint32_t BridgeClient::processId() const noexcept { return impl->pid.load(); }
std::string BridgeClient::endpoint() const { return impl->pipeName; }
Status BridgeClient::openEditor(uint64_t parentWindow, float scale)
{
    std::lock_guard lifecycle(impl->lifecycle);
    impl->editorParent = parentWindow; impl->editorScale = scale;
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
    try
    {
        std::lock_guard lifecycle(impl->lifecycle);
        impl->editorParent = 0;
        if (impl->ready) request(ControlCommand::closeEditor, {}, std::chrono::seconds(2));
    }
    catch (...) {}
}
Size BridgeClient::editorSize() const noexcept { return {impl->editorWidth.load(), impl->editorHeight.load()}; }
Status BridgeClient::prepare(const ProcessConfig& requested)
{
    std::lock_guard lifecycle(impl->lifecycle);
    std::unique_lock operation(impl->stateOperations);
    Impl::Resume resume {*impl};
    if (!impl->pause(std::chrono::seconds(10))) return Failure{ErrorCode::timeout, "Audio did not reach a preparation boundary"};
    auto config = requested;
    config.parameterCount = impl->parameterCount;
    { std::lock_guard lock(impl->snapshots); if (impl->binding) config.programCount = static_cast<uint32_t>(impl->binding->programs.names.size()); }
    if (impl->audio)
        for (auto event : impl->audio->parameterCheckpoint())
            if (!impl->parameterQueue->push(event)) impl->parameterOverflow = true;
    auto name = L"Local\\NyteraAudio-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(impl->pid.load())
        + L"-" + std::to_wstring(++impl->audioGeneration);
    auto created = AudioTransport::create(name, config);
    if (auto* failure = std::get_if<Failure>(&created)) return *failure;
    auto audio = std::get<std::unique_ptr<AudioTransport>>(std::move(created));
    audio->setStopEvent(impl->stopEvent.get());
    Writer writer;
    writer.text(pathUtf8(name)); writer.floating(config.sampleRate);
    writer.number(config.maximumBlock); writer.number(config.inputs); writer.number(config.outputs); writer.boolean(config.offline);
    auto connected = request(ControlCommand::connectAudio, writer.bytes);
    if (auto* failure = std::get_if<Failure>(&connected)) return *failure;
    try
    {
        Reader response(std::get<Bytes>(connected));
        auto sourceLatency = response.number<uint32_t>();
        auto tail = response.real64();
        response.finish();
        impl->totalLatency = audio->latency() + sourceLatency;
        impl->pipelineLatency = audio->latency();
        impl->tail = tail;
        impl->audio = std::move(audio);
        impl->preparedConfig = requested;
        {
            std::lock_guard lock(impl->snapshots);
            if (impl->completeState) impl->audio->restoreCheckpoint(impl->completeState->program, impl->completeState->parameters, impl->completeBarrier);
        }
        return std::monostate{};
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
ProcessResult BridgeClient::process(AudioBlockView block, EventBlockView events, const TransportSnapshot& transport) noexcept
{
    return impl->render(block, events, transport);
}
ProcessResult BridgeClient::process(DoubleAudioBlockView block, EventBlockView events, const TransportSnapshot& transport) noexcept
{
    return impl->render(block, events, transport);
}
uint32_t BridgeClient::latencySamples() const noexcept { return impl->totalLatency.load(); }
void BridgeClient::enqueueParameter(const ParameterEvent& event) noexcept
{
    auto ordered = event;
    ordered.sequence = ++impl->parameterSequence;
    if (!impl->parameterQueue->push(ordered)) impl->parameterOverflow = true;
}
Result<ParameterDisplay> BridgeClient::parameterDisplay(uint32_t index)
{
    Writer writer;
    writer.number(index);
    auto reply = request(ControlCommand::display, writer.bytes, std::chrono::seconds(2));
    if (auto* failure = std::get_if<Failure>(&reply)) return *failure;
    try { Reader reader(std::get<Bytes>(reply)); ParameterDisplay text {reader.real32(), reader.text()}; reader.finish(); return text; }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
Result<std::vector<float>> BridgeClient::parameterValues()
{
    Writer writer; writer.number<uint32_t>(0);
    auto reply = request(ControlCommand::parameter, writer.bytes, std::chrono::seconds(2));
    if (auto* failure = std::get_if<Failure>(&reply)) return *failure;
    try
    {
        Reader reader(std::get<Bytes>(reply));
        auto count = reader.count(65536);
        std::vector<float> values;
        for (uint32_t i = 0; i < count; ++i) values.push_back(reader.real32());
        reader.finish();
        return values;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
Status BridgeClient::flushParameters()
{
    Writer writer;
    std::vector<ParameterEvent> pending;
    pending.reserve(8192);
    ParameterEvent event;
    while (pending.size() < 8192 && impl->parameterQueue->pop(event)) pending.push_back(event);
    if (pending.empty()) return std::monostate{};
    writer.number(static_cast<uint32_t>(pending.size()));
    for (const auto& parameter : pending)
    {
        writer.number(parameter.index); writer.floating(parameter.normalized);
        writer.number(parameter.origin); writer.number(parameter.sequence);
    }
    auto reply = request(ControlCommand::parameter, writer.bytes, std::chrono::seconds(2));
    if (auto* failure = std::get_if<Failure>(&reply)) return *failure;
    return std::monostate{};
}
Status BridgeClient::releaseAudio()
{
    std::lock_guard lifecycle(impl->lifecycle);
    std::unique_lock operation(impl->stateOperations);
    Impl::Resume resume {*impl};
    if (!impl->pause(std::chrono::seconds(10))) return Failure{ErrorCode::timeout, "Audio did not reach a release boundary"};
    impl->preparedConfig.reset();
    auto response = request(ControlCommand::releaseAudio);
    if (auto* failure = std::get_if<Failure>(&response)) return *failure;
    impl->audio.reset();
    return std::monostate{};
}
void BridgeClient::resetProcessing() noexcept { if (impl->audio) impl->audio->reset(); }
Result<StateEnvelope> BridgeClient::captureState(std::chrono::milliseconds timeout)
{
    if (!impl->ready)
    {
        if (auto cached = lastState()) return *cached;
        return Failure{ErrorCode::disconnected, "Source state is unavailable before startup"};
    }
    Deadline deadline(timeout);
    std::unique_lock operation(impl->stateOperations, std::defer_lock);
    if (!operation.try_lock_for(deadline.remaining())) return Failure{ErrorCode::timeout, "Another state operation is pending"};
    auto checkpointSequence = ++impl->parameterSequence;
    Impl::Resume resume {*impl};
    if (!impl->pause(deadline.remaining())) return Failure{ErrorCode::timeout, "Audio processing did not reach a state boundary"};
    try
    {
        auto pending = impl->audio ? impl->audio->parameterCheckpoint() : std::vector<ParameterEvent>{};
        ParameterEvent event;
        for (uint32_t i = 0; i < 8192 && impl->parameterQueue->pop(event); ++i) pending.push_back(event);
        Writer writer;
        writer.number(static_cast<uint32_t>(pending.size()));
        for (const auto& parameter : pending)
        {
            writer.number(parameter.index); writer.floating(parameter.normalized);
            writer.number(parameter.origin); writer.number(parameter.sequence);
        }
        auto response = request(ControlCommand::captureState, writer.bytes, deadline.remaining());
        if (auto* failure = std::get_if<Failure>(&response)) return *failure;
        auto decoded = decodeState(std::get<Bytes>(response));
        if (auto* failure = std::get_if<Failure>(&decoded)) return *failure;
        auto state = std::get<StateEnvelope>(std::move(decoded));
        std::lock_guard lock(impl->snapshots);
        auto valid = validateStateBinding(state, *impl->binding);
        if (auto* failure = std::get_if<Failure>(&valid)) return *failure;
        impl->completeState = state;
        impl->completeBarrier = checkpointSequence;
        return state;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
Status BridgeClient::applyState(const StateEnvelope& state, uint64_t barrier, std::chrono::milliseconds timeout)
{
    if (barrier < impl->stateBarrier) return std::monostate{};
    try
    {
        auto bytes = encodeState(state);
        Writer writer;
        writer.number(barrier); writer.number(static_cast<uint32_t>(bytes.size())); writer.raw(bytes);
        auto response = request(ControlCommand::restoreState, writer.bytes, timeout);
        if (auto* failure = std::get_if<Failure>(&response)) return *failure;
        Reader reader(std::get<Bytes>(response));
        auto programs = decodePrograms(reader.raw(reader.count(16 * 1024 * 1024)));
        if (auto* failure = std::get_if<Failure>(&programs)) return *failure;
        auto restored = state;
        restored.program = std::get<ProgramInfo>(programs).current;
        restored.programName = std::get<ProgramInfo>(programs).names[restored.program].substr(0, 23);
        auto count = reader.count(65536);
        if (count != state.parameters.size()) throw std::runtime_error("Restored parameter topology differs from saved state");
        restored.parameters.clear();
        for (uint32_t i = 0; i < count; ++i) restored.parameters.push_back(reader.real32());
        auto latency = reader.number<uint32_t>(); auto tail = reader.real64(); reader.finish();
        validateState(restored);
        impl->totalLatency = impl->pipelineLatency.load() + latency; impl->tail = tail;
        if (impl->audio) impl->audio->restoreCheckpoint(restored.program, restored.parameters, barrier);
        impl->stateBarrier = barrier;
        std::lock_guard lock(impl->snapshots);
        impl->completeState = std::move(restored);
        impl->completeBarrier = barrier;
        return std::monostate{};
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
Status BridgeClient::restoreState(const StateEnvelope& state)
{
    try { validateState(state); }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
    auto barrier = ++impl->parameterSequence;
    {
        std::lock_guard lock(impl->snapshots);
        if (impl->binding)
        {
            auto valid = validateStateBinding(state, *impl->binding);
            if (auto* failure = std::get_if<Failure>(&valid)) return *failure;
        }
        if (!impl->ready)
        {
            impl->pendingState = state; impl->pendingBarrier = barrier;
            return std::monostate{};
        }
    }
    std::unique_lock operation(impl->stateOperations, std::defer_lock);
    Deadline deadline(std::chrono::seconds(10));
    if (!operation.try_lock_for(deadline.remaining())) return Failure{ErrorCode::timeout, "Another state operation is pending"};
    Impl::Resume resume {*impl};
    if (!impl->pause(deadline.remaining())) return Failure{ErrorCode::timeout, "Audio processing did not reach a state boundary"};
    if (impl->audio) impl->audio->reset();
    return applyState(state, barrier, deadline.remaining());
}
std::optional<StateEnvelope> BridgeClient::lastState() const
{
    std::lock_guard lock(impl->snapshots);
    if (impl->pendingState) return impl->pendingState;
    return impl->completeState;
}
Result<ProgramInfo> BridgeClient::programInfo()
{
    auto response = request(ControlCommand::programs, {}, std::chrono::seconds(2));
    if (auto* failure = std::get_if<Failure>(&response)) return *failure;
    return decodePrograms(std::get<Bytes>(response));
}
double BridgeClient::tailLengthSeconds() const noexcept { return impl->tail.load(); }
}
