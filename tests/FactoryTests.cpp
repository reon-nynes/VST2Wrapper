#include "Vst3Host.h"
#include "common/Manifest.h"
#include "package/Package.h"
#include "Test.h"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <windows.h>
#include <cstring>

using namespace nytera;
using namespace Steinberg;
using namespace Steinberg::Vst;

static std::pair<Manifest, std::filesystem::path> binding(int variant)
{
    Manifest manifest;
    manifest.displayName = variant == 1 ? "Nytera Test Alpha" : "Nytera Test Beta";
    manifest.dllName = "fixture.dll";
    manifest.sourceUniqueId = static_cast<uint32_t>(variant);
    manifest.parameters = {{0, 0, "Gain", "dB", 0.5f, true}};
    manifest.inputs = {{"Left"}, {"Right"}};
    manifest.outputs = manifest.inputs;
    manifest = assignBindingIds(manifest);
    auto folder = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out" / "factory-tests";
    std::filesystem::create_directories(folder);
    auto output = folder / ("Test" + std::to_string(variant) + ".vst3");
    require(writePackage({manifest, NYTERA_EFFECT_TEMPLATE, {}, {}, {}}, output));
    return {manifest, output};
}

TEST("Factory.DistinctBindings")
{
    auto [a, ap] = binding(1);
    auto [b, bp] = binding(2);
    FactoryModule first(ap), second(bp);
    auto ai = first.component(), bi = second.component();
    CHECK(std::memcmp(ai.cid, bi.cid, 16) != 0);
    CHECK(std::memcmp(ai.cid, a.processorId.data(), 16) == 0);
    CHECK(std::strcmp(ai.name, "Nytera Test Alpha") == 0);
}

TEST("Factory.ControllerIdentity")
{
    auto [manifest, path] = binding(1);
    FactoryModule loaded(path);
    auto info = loaded.component();
    IComponent* component = nullptr;
    CHECK(loaded.factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
    TUID controller;
    CHECK(component->getControllerClassId(controller) == kResultOk);
    CHECK(std::memcmp(controller, manifest.controllerId.data(), 16) == 0);
    IEditController* edit = nullptr;
    CHECK(loaded.factory->createInstance(controller, IEditController::iid, reinterpret_cast<void**>(&edit)) == kResultOk);
    edit->release();
    component->release();
}

static void headlessParameters(bool exactCount)
{
    auto [manifest, path] = binding(1);
    FactoryModule loaded(path);
    IEditController* edit = nullptr;
    IComponent* component = nullptr;
    auto info = loaded.component();
    CHECK(loaded.factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk);
    CHECK(component->initialize(nullptr) == kResultOk);
    CHECK(loaded.factory->createInstance(reinterpret_cast<const char*>(manifest.controllerId.data()), IEditController::iid, reinterpret_cast<void**>(&edit)) == kResultOk);
    CHECK(edit->initialize(nullptr) == kResultOk);
    IConnectionPoint* source = nullptr;
    IConnectionPoint* controller = nullptr;
    CHECK(component->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&source)) == kResultOk);
    CHECK(edit->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&controller)) == kResultOk);
    CHECK(source->connect(controller) == kResultOk);
    CHECK(controller->connect(source) == kResultOk);
    CHECK(edit->getParameterCount() >= 1);
    if (exactCount) CHECK(edit->getParameterCount() == static_cast<int32>(manifest.parameters.size() + 1));
    Steinberg::Vst::ParameterInfo parameter;
    CHECK(edit->getParameterInfo(0, parameter) == kResultOk);
    CHECK(parameter.id == manifest.parameters[0].vst3Id);
    controller->disconnect(source);
    source->disconnect(controller);
    controller->release();
    source->release();
    edit->terminate();
    edit->release();
    component->terminate();
    component->release();
}
TEST("Factory.HeadlessParameters") { headlessParameters(false); }
TEST("Parameter.StableIds") { headlessParameters(true); }

TEST("Factory.RenameStable")
{
    auto [manifest, path] = binding(1);
    auto renamed = path.parent_path() / "Renamed.vst3";
    std::filesystem::copy_file(path, renamed, std::filesystem::copy_options::overwrite_existing);
    FactoryModule loaded(renamed);
    CHECK(std::memcmp(loaded.component().cid, manifest.processorId.data(), 16) == 0);
}
