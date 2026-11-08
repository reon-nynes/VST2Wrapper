#include "Vst2Instance.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <iostream>
namespace nytera
{
static void trace(const char* stage)
{
    char enabled[2] {};
    if (GetEnvironmentVariableA("NYTERA_TRACE", enabled, sizeof(enabled))) std::clog << stage << std::endl;
}
thread_local Vst2Instance* Vst2Instance::constructing = nullptr;
Result<std::unique_ptr<Vst2Instance>> Vst2Instance::open(const std::filesystem::path& path)
{
    auto architecture = inspectPeArchitecture(path);
    if (auto* failure = std::get_if<Failure>(&architecture)) return *failure;
    constexpr auto nativeArchitecture = sizeof(void*) == 8 ? Architecture::x64 : Architecture::x86;
    if (std::get<Architecture>(architecture) != nativeArchitecture)
        return Failure{ErrorCode::architecture, "DLL architecture does not match helper architecture"};
    auto instance = std::unique_ptr<Vst2Instance>(new Vst2Instance);
    try
    {
        instance->source = std::filesystem::absolute(path);
        instance->sourceDirectory = instance->source.parent_path().string();
        trace("Loading DLL");
        instance->module = LoadLibraryExW(instance->source.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!instance->module) return Failure{ErrorCode::plugin, "Windows could not load DLL or dependency (error " + std::to_string(GetLastError()) + ")"};
        auto entry = reinterpret_cast<Vst2Entry>(GetProcAddress(instance->module, "VSTPluginMain"));
        if (!entry) entry = reinterpret_cast<Vst2Entry>(GetProcAddress(instance->module, "main"));
        if (!entry) return Failure{ErrorCode::invalidData, "DLL has no VST2 entry point"};
        auto* previous = constructing;
        constructing = instance.get();
        trace("Calling VST2 entry point");
        try { instance->effect = entry(host); }
        catch (...) { constructing = previous; throw; }
        constructing = previous;
        trace("VST2 entry point returned");
        auto* effect = instance->effect;
        if (!effect || effect->magic_number != VST_FOURCC('V', 's', 't', 'P') || !effect->control)
        {
            instance->effect = nullptr;
            return Failure{ErrorCode::invalidData, "Invalid VST2 instance or magic"};
        }
        effect->host_internal = instance.get();
        if (effect->num_params < 0 || effect->num_params > 65536 || effect->num_inputs < 0
            || effect->num_inputs > 256 || effect->num_outputs < 0 || effect->num_outputs > 256
            || (effect->num_params > 0 && (!effect->get_parameter || !effect->set_parameter))
            || (!effect->process_float && !effect->process && !effect->process_double))
            return Failure{ErrorCode::invalidData, "Invalid VST2 counts or function pointers"};
        instance->opened = true;
        trace("Opening VST2 instance");
        instance->dispatch(VST_EFFECT_OPCODE_CREATE);
        trace("Setting initial sample rate");
        instance->dispatch(VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr, 44100.0f);
        trace("Setting initial block size");
        instance->dispatch(VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, 512);
        trace("VST2 instance initialized");
        return instance;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::plugin, error.what()}; }
}
Vst2Instance::~Vst2Instance() { close(); }
Result<Manifest> Vst2Instance::inspect()
{
    trace("Inspecting VST2 metadata");
    if (!effect) return Failure{ErrorCode::plugin, "Plugin is closed"};
    if (!(effect->flags & VST_EFFECT_FLAG_EDITOR)) return Failure{ErrorCode::unsupported, "Plugin has no original editor"};
    if (dispatch(VST_EFFECT_OPCODE_CATEGORY) == VST_EFFECT_CATEGORY_CONTAINER)
        return Failure{ErrorCode::unsupported, "Multi-plugin shell DLLs require shell support"};
    auto text = [&](int32_t opcode, int32_t index = 0)
    {
        char buffer[1024] {};
        dispatch(opcode, index, 0, buffer);
        buffer[sizeof(buffer) - 1] = 0;
        return std::string(buffer);
    };
    try
    {
        Manifest manifest;
        manifest.sourceHash = sha256(readFile(source));
        manifest.dllName = pathUtf8(source.filename());
        manifest.displayName = text(VST_EFFECT_OPCODE_EFFECT_NAME);
        if (manifest.displayName.empty()) manifest.displayName = pathUtf8(source.stem());
        manifest.sourceVendor = text(VST_EFFECT_OPCODE_VENDOR_NAME);
        manifest.sourceUniqueId = static_cast<uint32_t>(effect->unique_id);
        manifest.architecture = sizeof(void*) == 8 ? Architecture::x64 : Architecture::x86;
        manifest.kind = (effect->flags & VST_EFFECT_FLAG_INSTRUMENT) ? PluginKind::instrument : PluginKind::effect;
        manifest.initialDelay = static_cast<uint32_t>(std::max(0, effect->delay));
        auto tail = dispatch(VST_EFFECT_OPCODE_TAIL_SAMPLES);
        manifest.tailSeconds = tail > 1 ? static_cast<double>(tail) / config.sampleRate : 0.0;
        for (int32_t i = 0; i < effect->num_params; ++i)
        {
            auto value = effect->get_parameter(effect, static_cast<uint32_t>(i));
            if (!std::isfinite(value)) return Failure{ErrorCode::invalidData, "Plugin returned a non-finite parameter"};
            manifest.parameters.push_back({static_cast<uint32_t>(i), 0, text(VST_EFFECT_OPCODE_PARAM_NAME, i),
                text(VST_EFFECT_OPCODE_PARAM_LABEL, i), std::clamp(value, 0.0f, 1.0f), dispatch(VST_EFFECT_OPCODE_PARAM_IS_AUTOMATABLE, i) != 0});
        }
        for (int32_t i = 0; i < effect->num_inputs; ++i) manifest.inputs.push_back({"Input " + std::to_string(i + 1)});
        for (int32_t i = 0; i < effect->num_outputs; ++i) manifest.outputs.push_back({"Output " + std::to_string(i + 1)});
        manifest.midiInput = manifest.kind == PluginKind::instrument || dispatch(VST_EFFECT_OPCODE_SUPPORTS, 0, 0, const_cast<char*>("receiveVstMidiEvent")) > 0;
        manifest.midiOutput = dispatch(VST_EFFECT_OPCODE_SUPPORTS, 0, 0, const_cast<char*>("sendVstMidiEvent")) > 0;
        vst_rect_t* rectangle = nullptr;
        dispatch(VST_EFFECT_OPCODE_EDITOR_GET_RECT, 0, 0, &rectangle);
        if (rectangle && rectangle->right > rectangle->left && rectangle->bottom > rectangle->top)
            manifest.editorSize = {static_cast<uint32_t>(rectangle->right - rectangle->left), static_cast<uint32_t>(rectangle->bottom - rectangle->top)};
        return assignBindingIds(std::move(manifest));
    }
    catch (const std::exception& error) { return Failure{ErrorCode::plugin, error.what()}; }
}
Status Vst2Instance::prepare(const ProcessConfig& next)
{
    if (!effect) return Failure{ErrorCode::plugin, "Plugin is closed"};
    if (!std::isfinite(next.sampleRate) || next.sampleRate < 1000 || next.sampleRate > 768000
        || next.maximumBlock == 0 || next.maximumBlock > 65536)
        return Failure{ErrorCode::invalidData, "Invalid processing configuration"};
    if (prepared)
    {
        dispatch(VST_EFFECT_OPCODE_PROCESS_END);
        dispatch(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0);
    }
    config = next;
    dispatch(VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr, static_cast<float>(config.sampleRate));
    dispatch(VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, config.maximumBlock);
    dispatch(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1);
    dispatch(VST_EFFECT_OPCODE_PROCESS_BEGIN);
    prepared = true;
    return std::monostate{};
}
void Vst2Instance::close() noexcept
{
    if (effect && opened)
    {
        try
        {
            if (prepared) { dispatch(VST_EFFECT_OPCODE_PROCESS_END); dispatch(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0); }
            dispatch(VST_EFFECT_OPCODE_DESTROY);
        }
        catch (...) {}
    }
    effect = nullptr;
    opened = prepared = false;
    if (module) { FreeLibrary(module); module = nullptr; }
}
intptr_t Vst2Instance::dispatch(int32_t opcode, int32_t index, intptr_t value, void* pointer, float real)
{
    return effect->control(effect, opcode, index, value, pointer, real);
}
intptr_t Vst2Instance::host(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t value, void* pointer, float real)
{
    auto* instance = constructing ? constructing : (effect ? static_cast<Vst2Instance*>(effect->host_internal) : nullptr);
    return instance ? instance->hostCall(opcode, index, value, pointer, real) : 0;
}
intptr_t Vst2Instance::hostCall(int32_t opcode, int32_t index, intptr_t value, void* pointer, float real)
{
    switch (opcode)
    {
        case VST_HOST_OPCODE_VST_VERSION: return 2400;
        case VST_HOST_OPCODE_GET_SAMPLE_RATE: return static_cast<intptr_t>(config.sampleRate);
        case VST_HOST_OPCODE_GET_BLOCK_SIZE: return config.maximumBlock;
        case VST_HOST_OPCODE_VENDOR_NAME:
        case VST_HOST_OPCODE_PRODUCT_NAME:
            if (pointer) std::memcpy(pointer, "Nytera", 7);
            return 1;
        case VST_HOST_OPCODE_VENDOR_VERSION: return 100;
        case VST_HOST_OPCODE_LANGUAGE: return 1;
        case VST_HOST_OPCODE_GET_EFFECT_DIRECTORY: return reinterpret_cast<intptr_t>(sourceDirectory.c_str());
        case VST_HOST_OPCODE_GET_ACTIVE_THREAD: return config.offline ? 4 : 2;
        case VST_HOST_OPCODE_SUPPORTS:
            if (pointer)
            {
                auto* capability = static_cast<const char*>(pointer);
                for (auto* supported : {"sizeWindow", "sendVstEvents", "sendVstMidiEvent", "receiveVstEvents", "receiveVstMidiEvent", "sendVstTimeInfo", "startStopProcess", "acceptIOChanges"})
                    if (std::strcmp(capability, supported) == 0) return 1;
            }
            return 0;
        case VST_HOST_OPCODE_AUTOMATE:
        case VST_HOST_OPCODE_PARAM_START_EDIT:
        case VST_HOST_OPCODE_PARAM_STOP_EDIT:
        case VST_HOST_OPCODE_IO_MODIFIED:
        case VST_HOST_OPCODE_EDITOR_RESIZE:
        case VST_HOST_OPCODE_EDITOR_UPDATE:
            if (onNotification) onNotification({static_cast<uint32_t>(opcode), index, value, real});
            return 1;
        default: return 0;
    }
}
}
