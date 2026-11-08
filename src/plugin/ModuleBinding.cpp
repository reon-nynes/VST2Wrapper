#include "package/Package.h"
#include "ModuleBinding.h"
#include "common/Codec.h"
extern "C" IMAGE_DOS_HEADER __ImageBase;
namespace nytera
{
HMODULE currentModule() { return reinterpret_cast<HMODULE>(&__ImageBase); }
const Manifest& moduleBinding()
{
    static const Manifest manifest = []
    {
        auto result = readModuleManifest(currentModule());
        if (auto* binding = std::get_if<Manifest>(&result)) return *binding;
        Manifest unbound;
        unbound.displayName = "Nytera Template";
        unbound.inputs = {{"Left"}, {"Right"}};
        unbound.outputs = unbound.inputs;
        return assignBindingIds(unbound);
    }();
    return manifest;
}
Id128 moduleInterfaceId(int interfaceType)
{
    if (interfaceType == 3) return moduleBinding().processorId;
    if (interfaceType == 1) return moduleBinding().controllerId;
    Writer seed;
    seed.raw(moduleBinding().conversionId);
    seed.number(interfaceType);
    auto digest = sha256(seed.bytes);
    Id128 result;
    std::copy_n(digest.begin(), result.size(), result.begin());
    return result;
}
const char* moduleDisplayName() { return moduleBinding().displayName.c_str(); }
uint32_t moduleParameterId(int index)
{
    const auto& parameters = moduleBinding().parameters;
    if (index < 0 || static_cast<size_t>(index) >= parameters.size()) return 0;
    return parameters[static_cast<size_t>(index)].vst3Id;
}
}
