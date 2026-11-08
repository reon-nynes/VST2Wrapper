#pragma once
#include "package/Package.h"
#include "Test.h"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
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
struct TestStateStream : Steinberg::IBStream
{
    nytera::Bytes bytes;
    Steinberg::int64 position = 0;
    Steinberg::uint32 references = 1;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID interfaceId, void** object) override
    {
        if (Steinberg::FUnknownPrivate::iidEqual(interfaceId, Steinberg::IBStream::iid)
            || Steinberg::FUnknownPrivate::iidEqual(interfaceId, Steinberg::FUnknown::iid))
        { *object = static_cast<Steinberg::IBStream*>(this); addRef(); return Steinberg::kResultOk; }
        *object = nullptr; return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++references; }
    Steinberg::uint32 PLUGIN_API release() override { return --references; }
    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 count, Steinberg::int32* actual) override
    {
        if (count < 0 || (!buffer && count)) return Steinberg::kInvalidArgument;
        auto available = bytes.size() - static_cast<size_t>(position);
        auto amount = std::min(available, static_cast<size_t>(count));
        if (amount) std::memcpy(buffer, bytes.data() + position, amount);
        position += static_cast<Steinberg::int64>(amount);
        if (actual) *actual = static_cast<Steinberg::int32>(amount);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 count, Steinberg::int32* actual) override
    {
        if (count < 0 || (!buffer && count)) return Steinberg::kInvalidArgument;
        auto end = static_cast<size_t>(position) + count;
        if (end > bytes.size()) bytes.resize(end);
        if (count) std::memcpy(bytes.data() + position, buffer, static_cast<size_t>(count));
        position += count;
        if (actual) *actual = count;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 offset, Steinberg::int32 mode, Steinberg::int64* result) override
    {
        auto next = offset;
        if (mode == kIBSeekCur) next += position;
        else if (mode == kIBSeekEnd) next += static_cast<Steinberg::int64>(bytes.size());
        else if (mode != kIBSeekSet) return Steinberg::kInvalidArgument;
        if (next < 0 || static_cast<uint64_t>(next) > bytes.size()) return Steinberg::kInvalidArgument;
        position = next;
        if (result) *result = position;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* result) override
    { if (!result) return Steinberg::kInvalidArgument; *result = position; return Steinberg::kResultOk; }
};
