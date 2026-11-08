#pragma once
#include "common/Manifest.h"
#include <windows.h>
namespace juce { class AudioProcessor; }
namespace nytera
{
const Manifest& moduleBinding();
Id128 moduleInterfaceId(int interfaceType);
const char* moduleDisplayName();
uint32_t moduleParameterId(int index);
HMODULE currentModule();
void moduleSetPrefetch(juce::AudioProcessor&, bool);
}
