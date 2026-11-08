#include <vst.h>
#include <windows.h>
#include <cstring>
#include <atomic>
#include <cwchar>
#include <array>
#include <cstdio>

// Independent pointer-sized callback declaration pins the x86 calling ABI.
using CanonicalHost = intptr_t (__cdecl*)(vst_effect_t*, int32_t, int32_t, intptr_t, void*, float);
struct FixtureCounters { uint32_t opens, closes, earlyCallbackOk; };
struct CanonicalEvents { int32_t count; intptr_t reserved; vst_event_t* events[4128]; };
struct CanonicalTime
{
    double samplePosition, sampleRate, nanoseconds, ppq, tempo, bar, cycleStart, cycleEnd;
    int32_t numerator, denominator, smpteOffset, frameRate, nextClock, flags;
};
static FixtureCounters counters {};
#ifndef NYTERA_FIXTURE_OPTIONS
#define NYTERA_FIXTURE_OPTIONS 0
#endif
static uint32_t options = NYTERA_FIXTURE_OPTIONS;
struct Instance
{
    vst_effect_t effect {};
    CanonicalHost host = nullptr;
    uint32_t flags = 0;
    std::atomic<float> gain {0.5f};
    std::atomic<uint32_t> currentProgram {0};
    std::array<std::atomic<float>, 2> programGains {0.5f, 0.25f};
    char programNames[2][32] {"Fixture A", "Fixture B"};
    std::array<std::byte, 32> stateBlob;
    bool stallCapture = false;
    bool slowCapture = false;
    bool crashCapture = false;
    bool stallEditor = false;
    vst_rect_t rectangle {0, 0, 160, 320};
    HWND editor = nullptr;
    struct Note { int32_t offset; uint8_t status, data1, data2; };
    std::array<Note, 4128> notes;
    uint32_t noteCount = 0;
    bool noteActive = false;
    std::array<std::array<float, 16>, 2> delayed {};
    uint32_t delayPosition = 0;
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
        instance->programGains[instance->currentProgram.load()] = instance->gain.load();
        instance->host(&instance->effect, 0, 0, 0, nullptr, instance->gain.load());
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
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 103)
    {
        instance->effect.num_outputs = 3;
        instance->host(&instance->effect, 13, 0, 0, nullptr, 0);
        return 0;
    }
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 104)
    {
        instance->effect.delay = 32;
        instance->host(&instance->effect, 13, 0, 0, nullptr, 0);
        return 0;
    }
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 106)
    {
        instance->host(&instance->effect, 43, 0, 0, nullptr, 0);
        instance->gain = 0.65f;
        instance->host(&instance->effect, 0, 0, 0, nullptr, 0.65f);
        return 0;
    }
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 107)
    {
        instance->stallCapture = true;
        return 0;
    }
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 108)
    {
        instance->slowCapture = true;
        return 0;
    }
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 109)
    {
        instance->crashCapture = true;
        return 0;
    }
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 110)
    {
        instance->stallEditor = true;
        return 0;
    }
    if (instance && message == WM_COMMAND && LOWORD(wParam) == 112)
    {
        instance->flags |= 64;
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
        swprintf_s(value, L"Nytera fixture original editor\nGain %.2f\nClick or type to edit", instance->gain.load());
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, value, -1, &rectangle, DT_CENTER | DT_WORDBREAK);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static intptr_t __cdecl control(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t value, void* pointer, float)
{
    auto& instance = *static_cast<Instance*>(effect->effect_internal);
    switch (opcode)
    {
        case VST_EFFECT_OPCODE_CREATE: ++counters.opens; return 1;
        case VST_EFFECT_OPCODE_DESTROY: ++counters.closes; delete &instance; return 1;
        case VST_EFFECT_OPCODE_EFFECT_NAME:
            return copyText(pointer, (instance.flags & 16) ? "Nytera MIDI Fixture" : (instance.flags & 128) ? "Nytera Delay Fixture"
                : (instance.flags & 8) ? "Nytera Chunk Fixture" : (instance.flags & 2048) ? "Nytera Program Fixture" : "Nytera Fixture");
        case VST_EFFECT_OPCODE_PROGRAM_GET: return instance.currentProgram.load();
        case VST_EFFECT_OPCODE_PROGRAM_SET:
            if (value < 0 || value >= effect->num_programs) return 0;
            instance.currentProgram = static_cast<uint32_t>(value);
            instance.gain = instance.programGains[static_cast<size_t>(value)].load();
            instance.host(effect, 0, 0, 0, nullptr, instance.gain.load());
            return 1;
        case VST_EFFECT_OPCODE_PROGRAM_GET_NAME: return copyText(pointer, instance.programNames[instance.currentProgram.load()]);
        case VST_EFFECT_OPCODE_PROGRAM_SET_NAME:
            std::strncpy(instance.programNames[instance.currentProgram.load()], static_cast<const char*>(pointer), 31);
            instance.programNames[instance.currentProgram.load()][31] = 0;
            return 1;
        case 29:
            return value >= 0 && value < effect->num_programs ? copyText(pointer, instance.programNames[value]) : 0;
        case VST_EFFECT_OPCODE_SUSPEND_RESUME:
            if (value && (instance.flags & 8192)) instance.gain = 0.5f;
            return 1;
        case VST_EFFECT_OPCODE_GET_CHUNK_DATA:
        {
            if (!(instance.flags & 8)) return 0;
            if (instance.stallCapture) Sleep(INFINITE);
            if (instance.crashCapture) TerminateProcess(GetCurrentProcess(), 33);
            if (instance.slowCapture) Sleep(100);
            if (index == 0 && (instance.flags & 16384)) return 0;
            instance.stateBlob.fill(std::byte{0xa5});
            uint32_t magic = 0x4346594e, program = instance.currentProgram.load();
            float gain = instance.gain.load();
            std::memcpy(instance.stateBlob.data(), &magic, 4);
            std::memcpy(instance.stateBlob.data() + 4, &gain, 4);
            std::memcpy(instance.stateBlob.data() + 8, &program, 4);
            *static_cast<void**>(pointer) = instance.stateBlob.data();
            return instance.stateBlob.size();
        }
        case VST_EFFECT_OPCODE_SET_CHUNK_DATA:
        {
            if (value != instance.stateBlob.size() || !pointer) return -1;
            uint32_t magic, program;
            float gain;
            auto* bytes = static_cast<const std::byte*>(pointer);
            std::memcpy(&magic, bytes, 4); std::memcpy(&gain, bytes + 4, 4); std::memcpy(&program, bytes + 8, 4);
            if (magic != 0x4346594e || program >= static_cast<uint32_t>(effect->num_programs)) return -1;
            instance.currentProgram = program; instance.gain = gain; instance.programGains[program] = gain;
            return 1;
        }
        case VST_EFFECT_OPCODE_VENDOR_NAME: return copyText(pointer, "Nytera");
        case VST_EFFECT_OPCODE_PARAM_NAME: return copyText(pointer, index == 0 ? "Gain" : "Mode");
        case VST_EFFECT_OPCODE_PARAM_LABEL: return copyText(pointer, "");
        case VST_EFFECT_OPCODE_PARAM_VALUE:
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.2f", instance.gain.load());
            return copyText(pointer, text);
        }
        case VST_EFFECT_OPCODE_PARAM_IS_AUTOMATABLE: return 1;
        case VST_EFFECT_OPCODE_CATEGORY: return (instance.flags & 2) ? VST_EFFECT_CATEGORY_CONTAINER : 1;
        case VST_EFFECT_OPCODE_EDITOR_GET_RECT: *static_cast<vst_rect_t**>(pointer) = &instance.rectangle; return 1;
        case VST_EFFECT_OPCODE_EDITOR_OPEN:
            if (instance.stallEditor) Sleep(INFINITE);
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
        case VST_EFFECT_OPCODE_TAIL_SAMPLES: return (instance.flags & 512) ? INT32_MAX : (instance.flags & 4096) ? 48000 : 1;
        case 50: // effVendorSpecific is used only by the controlled fixture.
        {
            auto* time = reinterpret_cast<CanonicalTime*>(instance.host(effect, 7, 0, 0xffff, nullptr, 0));
            return time ? static_cast<intptr_t>(index == 1 ? time->tempo : time->samplePosition) : -1;
        }
        case VST_EFFECT_OPCODE_SUPPORTS:
            return (instance.flags & 16) && pointer
                && (std::strcmp(static_cast<const char*>(pointer), "receiveVstMidiEvent") == 0 || std::strcmp(static_cast<const char*>(pointer), "sendVstMidiEvent") == 0) ? 1 : 0;
        case VST_EFFECT_OPCODE_EVENT:
        {
            auto& incoming = *static_cast<CanonicalEvents*>(pointer);
            instance.noteCount = 0;
            for (int32_t i = 0; i < incoming.count && i < 4128; ++i)
            {
                auto* event = incoming.events[i];
                if (event->type != VST_EVENT_TYPE_MIDI) continue;
                auto* midi = reinterpret_cast<vst_event_midi_t*>(event);
                instance.notes[instance.noteCount++] = {event->offset, static_cast<uint8_t>(midi->midi.data[0]),
                    static_cast<uint8_t>(midi->midi.data[1]), static_cast<uint8_t>(midi->midi.data[2])};
            }
            if (instance.flags & 16) instance.host(effect, 9, 0, 0, pointer, 0);
            return 1;
        }
        default: return 0;
    }
}
static void __cdecl process(vst_effect_t* effect, const float* const* inputs, float** outputs, int32_t count)
{
    auto& instance = *static_cast<Instance*>(effect->effect_internal);
    if (instance.flags & 32) RaiseException(0xc0000005, 0, 0, nullptr);
    if (instance.flags & 64) Sleep(INFINITE);
    if (instance.flags & 65536)
    {
        vst_event_midi_t message {};
        message.event.type = VST_EVENT_TYPE_MIDI; message.event.size = sizeof(message);
        message.event.offset = count + 1; message.midi.data[0] = static_cast<char>(0x90);
        CanonicalEvents outgoing {}; outgoing.count = 1; outgoing.events[0] = &message.event;
        instance.host(effect, 9, 0, 0, &outgoing, 0);
    }
    auto* time = (instance.flags & 1024) ? reinterpret_cast<CanonicalTime*>(instance.host(effect, 7, 0, 0xffff, nullptr, 0)) : nullptr;
    uint32_t noteIndex = 0;
    for (int32_t i = 0; i < count; ++i)
    {
        while (noteIndex < instance.noteCount && instance.notes[noteIndex].offset <= i)
        {
            const auto& note = instance.notes[noteIndex++];
            if ((note.status & 0xf0) == 0x90) instance.noteActive = note.data2 != 0;
            if ((note.status & 0xf0) == 0x80 || ((note.status & 0xf0) == 0xb0 && (note.data1 == 120 || note.data1 == 123))) instance.noteActive = false;
        }
        for (int32_t channel = 0; channel < effect->num_outputs; ++channel)
        {
            float value = (instance.flags & 16) ? (instance.noteActive ? instance.gain.load() : 0.0f) : inputs[channel][i] * instance.gain.load();
            if (instance.flags & 128)
            {
                float previous = instance.delayed[static_cast<size_t>(channel)][instance.delayPosition];
                instance.delayed[static_cast<size_t>(channel)][instance.delayPosition] = value;
                value = previous;
            }
            if (instance.flags & 1024)
                value = time && time->sampleRate == 48000 && time->tempo == 123 && time->numerator == 7 && time->denominator == 8
                    && (time->flags & (1024 | 8192)) == (1024 | 8192) ? static_cast<float>(time->samplePosition + i) : -999.0f;
            outputs[channel][i] = value;
        }
        instance.delayPosition = (instance.delayPosition + 1) % 16;
    }
    instance.noteCount = 0;
}
static void __cdecl setParameter(vst_effect_t* effect, uint32_t, float value)
{
    auto* instance = static_cast<Instance*>(effect->effect_internal);
    instance->gain = value;
    instance->programGains[instance->currentProgram.load()] = value;
    instance->host(effect, 0, 0, 0, nullptr, value);
}
static float __cdecl getParameter(vst_effect_t* effect, uint32_t)
{
    return static_cast<Instance*>(effect->effect_internal)->gain.load();
}
extern "C" __declspec(dllexport) vst_effect_t* VSTPluginMain(CanonicalHost host)
{
    if (options & 262144) Sleep(INFINITE);
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
    effect.num_programs = (options & 2048) ? 2 : 1;
    effect.num_params = (options & 32768) ? 9000 : 1;
    effect.num_inputs = (options & 16) ? 0 : 2;
    effect.num_outputs = 2;
    effect.flags = VST_EFFECT_FLAG_SUPPORTS_FLOAT | ((options & 1) ? 0 : VST_EFFECT_FLAG_EDITOR)
        | ((options & 16) ? VST_EFFECT_FLAG_INSTRUMENT : 0) | ((options & 8) ? VST_EFFECT_FLAG_CHUNKS : 0);
    effect.delay = (options & 128) ? 16 : 0;
    effect.effect_internal = instance;
    effect.unique_id = 0x4e795431;
    effect.version = 1;
    return &effect;
}
