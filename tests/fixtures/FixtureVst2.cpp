#include <vst.h>
#include <windows.h>
#include <cstring>
#include <atomic>
#include <cwchar>

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
    HWND editor = nullptr;
};

extern "C" __declspec(dllexport) void FixtureConfigure(uint32_t flags) { options = flags; counters = {}; }
extern "C" __declspec(dllexport) FixtureCounters FixtureStats() { return counters; }
static intptr_t copyText(void* destination, const char* text)
{
    std::memcpy(destination, text, std::strlen(text) + 1);
    return 1;
}
static LRESULT CALLBACK editorProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* instance = reinterpret_cast<Instance*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        instance = static_cast<Instance*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
    }
    if (instance && (message == WM_LBUTTONDOWN || message == WM_KEYDOWN))
    {
        SetFocus(window);
        instance->host(&instance->effect, 43, 0, 0, nullptr, 0);
        instance->gain = instance->gain < 0.75f ? instance->gain + 0.25f : 0.0f;
        instance->host(&instance->effect, 0, 0, 0, nullptr, instance->gain);
        instance->host(&instance->effect, 44, 0, 0, nullptr, 0);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 102)
    {
        instance->rectangle = {0, 0, 200, 400};
        SetWindowPos(window, nullptr, 0, 0, 400, 200, SWP_NOZORDER | SWP_NOACTIVATE);
        instance->host(&instance->effect, 15, 400, 200, nullptr, 0);
        return 0;
    }
    if (instance && message == WM_PAINT)
    {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT rectangle;
        GetClientRect(window, &rectangle);
        FillRect(dc, &rectangle, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
        wchar_t value[128];
        swprintf_s(value, L"Nytera fixture original editor\nGain %.2f\nClick or type to edit", instance->gain);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, value, -1, &rectangle, DT_CENTER | DT_WORDBREAK);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static intptr_t __cdecl control(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t, void* pointer, float)
{
    auto& instance = *static_cast<Instance*>(effect->effect_internal);
    switch (opcode)
    {
        case VST_EFFECT_OPCODE_CREATE: ++counters.opens; return 1;
        case VST_EFFECT_OPCODE_DESTROY: ++counters.closes; delete &instance; return 1;
        case VST_EFFECT_OPCODE_EFFECT_NAME: return copyText(pointer, "Nytera Fixture");
        case VST_EFFECT_OPCODE_VENDOR_NAME: return copyText(pointer, "Nytera");
        case VST_EFFECT_OPCODE_PARAM_NAME: return copyText(pointer, index == 0 ? "Gain" : "Mode");
        case VST_EFFECT_OPCODE_PARAM_LABEL: return copyText(pointer, "");
        case VST_EFFECT_OPCODE_PARAM_VALUE: return copyText(pointer, "0.5");
        case VST_EFFECT_OPCODE_PARAM_IS_AUTOMATABLE: return 1;
        case VST_EFFECT_OPCODE_CATEGORY: return (instance.flags & 2) ? VST_EFFECT_CATEGORY_CONTAINER : 1;
        case VST_EFFECT_OPCODE_EDITOR_GET_RECT: *static_cast<vst_rect_t**>(pointer) = &instance.rectangle; return 1;
        case VST_EFFECT_OPCODE_EDITOR_OPEN:
        {
            WNDCLASSW type {};
            type.lpfnWndProc = editorProcedure;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"NyteraFixtureEditor";
            type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&type);
            instance.editor = CreateWindowExW(0, type.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, instance.rectangle.right, instance.rectangle.bottom, static_cast<HWND>(pointer), nullptr, type.hInstance, &instance);
            return instance.editor ? 1 : 0;
        }
        case VST_EFFECT_OPCODE_EDITOR_CLOSE:
            if (instance.editor) { DestroyWindow(instance.editor); instance.editor = nullptr; }
            return 1;
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
    effect.magic_number = (options & 4) || !counters.earlyCallbackOk ? 0 : VST_FOURCC('V', 's', 't', 'P');
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
