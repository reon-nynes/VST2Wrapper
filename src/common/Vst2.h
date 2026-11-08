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
}
