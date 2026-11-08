#include "package/Package.h"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <cstring>
#include <iostream>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) { std::cerr << "Usage: NyteraFactoryProbe <plugin.vst3>\n"; return 2; }
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) { std::cerr << "Load failed " << GetLastError() << '\n'; return 1; }
    auto init = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
    auto entry = reinterpret_cast<Steinberg::IPluginFactory* (PLUGIN_API*)()>(GetProcAddress(module, "GetPluginFactory"));
    if (!init || !entry || !init()) { FreeLibrary(module); return 1; }
    auto* factory = entry();
    bool found = false;
    for (Steinberg::int32 index = 0; index < factory->countClasses(); ++index)
    {
        Steinberg::PClassInfo info;
        if (factory->getClassInfo(index, &info) == Steinberg::kResultOk)
        {
            std::cout << info.name << " " << info.category << " "
                      << nytera::hex(std::as_bytes(std::span(info.cid))) << '\n';
            found |= std::strcmp(info.category, kVstAudioEffectClass) == 0;
        }
    }
    factory->release();
    auto exit = reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
    if (exit) exit();
    FreeLibrary(module);
    return found ? 0 : 1;
}
