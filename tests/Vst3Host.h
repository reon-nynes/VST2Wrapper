#pragma once
#include "package/Package.h"
#include "Test.h"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <cstring>
struct FactoryModule
{
    HMODULE module = nullptr;
    Steinberg::IPluginFactory* factory = nullptr;
    explicit FactoryModule(const std::filesystem::path& path)
    {
        module = LoadLibraryW(path.c_str());
        if (!module) throw std::runtime_error("Cannot load generated VST3 " + std::to_string(GetLastError()));
        auto init = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
        CHECK(init && init());
        auto entry = reinterpret_cast<Steinberg::IPluginFactory* (PLUGIN_API*)()>(GetProcAddress(module, "GetPluginFactory"));
        CHECK(entry);
        factory = entry();
        CHECK(factory);
    }
    ~FactoryModule()
    {
        if (factory) factory->release();
        if (module)
        {
            auto exit = reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
            if (exit) exit();
            FreeLibrary(module);
        }
    }
    Steinberg::PClassInfo component()
    {
        for (Steinberg::int32 i = 0; i < factory->countClasses(); ++i)
        {
            Steinberg::PClassInfo info;
            CHECK(factory->getClassInfo(i, &info) == Steinberg::kResultOk);
            if (std::strcmp(info.category, kVstAudioEffectClass) == 0) return info;
        }
        throw std::runtime_error("Missing component");
    }
};
