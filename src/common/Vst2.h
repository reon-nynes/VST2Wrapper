#pragma once
#include <vst.h>
#include <cstddef>

namespace nytera
{
// The pinned SDK declares the callback value as int64_t on x86. The VST2
// wire ABI uses a pointer-sized integer, just like the effect dispatcher.
using Vst2HostCallback = intptr_t (VST_FUNCTION_INTERFACE*)(vst_effect_t*, int32_t, int32_t, intptr_t, void*, float);
using Vst2Entry = vst_effect_t* (VST_FUNCTION_INTERFACE*)(Vst2HostCallback);
static_assert(sizeof(vst_effect_t) == (sizeof(void*) == 8 ? 192 : 144));
static_assert(offsetof(vst_effect_t, process_float) == (sizeof(void*) == 8 ? 120 : 80));
// VstEvents has an inline pointer array and pointer-sized reserved field.
// The pinned clean-room header models this container as a separate pointer.
struct Vst2Events
{
    int32_t count = 0;
    intptr_t reserved = 0;
    vst_event_t* events[4128] {};
};
static_assert(offsetof(Vst2Events, events) == (sizeof(void*) == 8 ? 16 : 8));
static_assert(sizeof(vst_event_midi_t) == 32);
static_assert(sizeof(vst_event_midi_sysex_t) == (sizeof(void*) == 8 ? 48 : 32));
struct Vst2TimeInfo
{
    double samplePosition = 0, sampleRate = 44100, nanoseconds = 0, ppq = 0, tempo = 120,
        barStart = 0, cycleStart = 0, cycleEnd = 0;
    int32_t numerator = 4, denominator = 4, smpteOffset = 0, frameRate = 0, samplesToNextClock = 0, flags = 0;
};
static_assert(sizeof(Vst2TimeInfo) == 88);
}
