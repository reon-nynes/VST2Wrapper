#pragma once
#include "common/Manifest.h"
#include "common/Vst2.h"
#include "common/Processing.h"
#include <windows.h>
#include <memory>
#include <functional>

namespace nytera
{
class Vst2Instance
{
public:
    static Result<std::unique_ptr<Vst2Instance>> open(const std::filesystem::path&);
    ~Vst2Instance();
    Result<Manifest> inspect();
    Status prepare(const ProcessConfig&);
    void close() noexcept;
    intptr_t dispatch(int32_t opcode, int32_t index = 0, intptr_t value = 0, void* pointer = nullptr, float real = 0);
    vst_effect_t* native() const { return effect; }
    std::function<void(const HostNotification&)> onNotification;
private:
    Vst2Instance() = default;
    static intptr_t VST_FUNCTION_INTERFACE host(vst_effect_t*, int32_t, int32_t, intptr_t, void*, float);
    intptr_t hostCall(int32_t, int32_t, intptr_t, void*, float);
    HMODULE module = nullptr;
    vst_effect_t* effect = nullptr;
    std::filesystem::path source;
    std::string sourceDirectory;
    ProcessConfig config;
    bool opened = false, prepared = false;
    static thread_local Vst2Instance* constructing;
};
}
