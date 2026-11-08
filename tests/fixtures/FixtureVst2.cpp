#include <vst.h>
#include <windows.h>
#include <cstring>
#include <atomic>

// Independent pointer-sized callback declaration pins the x86 calling ABI.
using CanonicalHost = intptr_t (__cdecl*)(vst_effect_t*, int32_t, int32_t, intptr_t, void*, float);
struct FixtureCounters { uint32_t opens, closes, earlyCallbackOk; };
static FixtureCounters counters {};
static uint32_t options = 0;
struct Instance
{
    vst_effect_t effect {};
    CanonicalHost host = nullptr;
    uint32_t flags = 0;
    float gain = 0.5f;
    vst_rect_t rectangle {0, 0, 160, 320};
};

extern "C" __declspec(dllexport) void FixtureConfigure(uint32_t flags) { options = flags; counters = {}; }
extern "C" __declspec(dllexport) FixtureCounters FixtureStats() { return counters; }

static intptr_t __cdecl control(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t, void* pointer, float)
{
    auto& instance = *static_cast<Instance*>(effect->effect_internal);
    switch (opcode)
    {
        case VST_EFFECT_OPCODE_CREATE: ++counters.opens; return 1;
        case VST_EFFECT_OPCODE_DESTROY: ++counters.closes; delete &instance; return 1;
        case VST_EFFECT_OPCODE_EFFECT_NAME: std::strcpy(static_cast<char*>(pointer), "Nytera Fixture"); return 1;
        case VST_EFFECT_OPCODE_VENDOR_NAME: std::strcpy(static_cast<char*>(pointer), "Nytera"); return 1;
        case VST_EFFECT_OPCODE_PARAM_NAME: std::strcpy(static_cast<char*>(pointer), index == 0 ? "Gain" : "Mode"); return 1;
        case VST_EFFECT_OPCODE_PARAM_LABEL: std::strcpy(static_cast<char*>(pointer), ""); return 1;
        case VST_EFFECT_OPCODE_PARAM_VALUE: std::strcpy(static_cast<char*>(pointer), "0.5"); return 1;
        case VST_EFFECT_OPCODE_PARAM_IS_AUTOMATABLE: return 1;
        case VST_EFFECT_OPCODE_CATEGORY: return (instance.flags & 2) ? VST_EFFECT_CATEGORY_CONTAINER : 1;
        case VST_EFFECT_OPCODE_EDITOR_GET_RECT: *static_cast<vst_rect_t**>(pointer) = &instance.rectangle; return 1;
        case VST_EFFECT_OPCODE_VST_VERSION: return 2400;
        default: return 0;
    }
}
static void __cdecl process(vst_effect_t* effect, const float* const* inputs, float** outputs, int32_t count)
{
    auto& instance = *static_cast<Instance*>(effect->effect_internal);
    if (instance.flags & 32) RaiseException(0xc0000005, 0, 0, nullptr);
    if (instance.flags & 64) Sleep(INFINITE);
    for (int32_t channel = 0; channel < effect->num_outputs; ++channel)
        for (int32_t i = 0; i < count; ++i) outputs[channel][i] = inputs[channel][i] * instance.gain;
}
static void __cdecl setParameter(vst_effect_t* effect, uint32_t, float value)
{
    static_cast<Instance*>(effect->effect_internal)->gain = value;
}
static float __cdecl getParameter(vst_effect_t* effect, uint32_t)
{
    return static_cast<Instance*>(effect->effect_internal)->gain;
}
extern "C" __declspec(dllexport) vst_effect_t* VSTPluginMain(CanonicalHost host)
{
    char vendor[64] {};
    auto version = host(nullptr, 1, 0, 0, nullptr, 0);
    auto vendorResult = host(nullptr, 32, 0, 0, vendor, 0);
    auto supports = host(nullptr, 37, 0, 0, const_cast<char*>("sizeWindow"), 0);
    counters.earlyCallbackOk = version == 2400 && vendorResult == 1 && std::strcmp(vendor, "Nytera") == 0 && supports == 1;
    auto* instance = new Instance;
    instance->host = host;
    instance->flags = options;
    auto& effect = instance->effect;
    effect.magic_number = (options & 4) ? 0 : VST_FOURCC('V', 's', 't', 'P');
    effect.control = control;
    effect.process_float = process;
    effect.set_parameter = setParameter;
    effect.get_parameter = getParameter;
    effect.num_programs = 1;
    effect.num_params = 1;
    effect.num_inputs = 2;
    effect.num_outputs = 2;
    effect.flags = VST_EFFECT_FLAG_SUPPORTS_FLOAT | ((options & 1) ? 0 : VST_EFFECT_FLAG_EDITOR);
    effect.effect_internal = instance;
    effect.unique_id = 0x4e795431;
    effect.version = 1;
    return &effect;
}
