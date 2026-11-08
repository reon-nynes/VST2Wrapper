#include "helper/Vst2Instance.h"
#include "Test.h"
#include <cmath>
using namespace nytera;
struct FixtureCounters { uint32_t opens, closes, earlyCallbackOk; };
struct Fixture
{
    HMODULE module = LoadLibraryW(std::filesystem::path(NYTERA_FIXTURE).c_str());
    explicit Fixture(uint32_t flags = 0)
    {
        CHECK(module);
        auto configure = reinterpret_cast<void (*)(uint32_t)>(GetProcAddress(module, "FixtureConfigure"));
        CHECK(configure);
        configure(flags);
    }
    FixtureCounters stats() { return reinterpret_cast<FixtureCounters (*)()>(GetProcAddress(module, "FixtureStats"))(); }
    ~Fixture() { if (module) FreeLibrary(module); }
};
TEST("Vst2.EntryCallback")
{
    Fixture fixture;
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    CHECK(fixture.stats().earlyCallbackOk == 1);
    auto manifest = require(instance->inspect());
    CHECK(manifest.displayName == "Nytera Fixture");
    CHECK(manifest.parameters.size() == 1);
}
TEST("Vst2.TailSemantics")
{
    {
        Fixture fixture;
        auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
        CHECK(instance->tailSeconds() == 0);
    }
    {
        Fixture fixture(512);
        auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
        CHECK(std::isinf(instance->tailSeconds()));
    }
    {
        Fixture fixture(4096);
        auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
        require(instance->prepare({48000, 64, 2, 2, false}));
        CHECK(instance->tailSeconds() == 1);
    }
}
TEST("Vst2.InvalidDll")
{
    auto result = Vst2Instance::open(std::filesystem::path(NYTERA_SOURCE_ROOT) / "missing.dll");
    CHECK(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == ErrorCode::invalidData);
}
TEST("Vst2.WrongArchitecture")
{
    auto file = std::filesystem::path(NYTERA_SOURCE_ROOT) / "vst2demo" / (sizeof(void*) == 8 ? "Decapitator.dll" : "VPRE-73.dll");
    auto result = Vst2Instance::open(file);
    CHECK(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).code == ErrorCode::architecture);
}
TEST("Vst2.NoEditor")
{
    Fixture fixture(1);
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    CHECK(std::holds_alternative<Failure>(instance->inspect()));
}
TEST("Vst2.Shell")
{
    Fixture fixture(2);
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    CHECK(std::holds_alternative<Failure>(instance->inspect()));
}
TEST("Vst2.CloseOnce")
{
    Fixture fixture;
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    instance->close(); instance->close(); instance.reset();
    CHECK(fixture.stats().opens == 1);
    CHECK(fixture.stats().closes == 1);
}
