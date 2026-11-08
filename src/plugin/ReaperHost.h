#pragma once
#include <pluginterfaces/base/funknown.h>
namespace nytera
{
// REAPER's published VST3 host extension ABI.
// https://github.com/justinfrankel/reaper-sdk/blob/main/sdk/reaper_vst3_interfaces.h
class ReaperHostApplication : public Steinberg::FUnknown
{
public:
    virtual void* PLUGIN_API getReaperApi(const char* name) = 0;
    virtual void* PLUGIN_API getReaperParent(Steinberg::uint32 kind) = 0;
    virtual void* PLUGIN_API reaperExtended(Steinberg::uint32 call, void*, void*, void*) = 0;
    inline static const Steinberg::FUID iid {0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C};
};
}
