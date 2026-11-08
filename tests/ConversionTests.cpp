#include "generator/Conversion.h"
#include "package/Package.h"
#include "Vst3Host.h"
#include "Test.h"
using namespace nytera;
static RuntimeAssets conversionAssets()
{
    auto root = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build";
    return {root / "x86/" NYTERA_TEST_CONFIG "/NyteraBridge.exe", root / "x64/" NYTERA_TEST_CONFIG "/NyteraBridge.exe", NYTERA_EFFECT_TEMPLATE, NYTERA_INSTRUMENT_TEMPLATE};
}
static std::filesystem::path conversionSource(const char* architecture = "x64")
{
    return std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build" / architecture / NYTERA_TEST_CONFIG "/NyteraFixture.dll";
}
static std::filesystem::path conversionOutput()
{
    auto folder = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/conversion-tests" / std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(folder); return folder / "Converted.vst3";
}
TEST("Conversion.MatchedHelper")
{
    auto assets = conversionAssets();
    for (const char* architecture : {"x86", "x64"})
    {
        auto inspected = require(inspectSource(conversionSource(architecture), assets));
        auto expected = std::string(architecture) == "x86" ? Architecture::x86 : Architecture::x64;
        CHECK(inspected.manifest.architecture == expected && inspected.helper == assets.helper(expected));
        CHECK(!inspected.imports.empty() && !inspected.report.empty());
        CHECK(outputFilename(inspected.manifest) == "Nytera Fixture.vst3");
    }
}
TEST("Conversion.MetadataMatchesOutput")
{
    auto assets = conversionAssets(); auto source = conversionSource();
    auto inspected = require(inspectSource(source, assets)); auto output = conversionOutput();
    require(generateConversion({source, {}, output, inspected}, assets));
    auto manifest = require(readPackageManifest(output));
    CHECK(manifest.processorId == inspected.manifest.processorId && manifest.parameters == inspected.manifest.parameters);
    CHECK(manifest.payloads.size() == 2);
    FactoryModule factory(output);
    auto component = factory.component();
    CHECK(std::memcmp(component.cid, manifest.processorId.data(), 16) == 0);
    CHECK(std::string(component.name) == manifest.displayName);
}
TEST("Conversion.NoCompiler")
{
    wchar_t original[32768]; auto length = GetEnvironmentVariableW(L"PATH", original, 32768);
    struct RestorePath { std::wstring path; ~RestorePath() { SetEnvironmentVariableW(L"PATH", path.c_str()); } } restore {std::wstring(original, length)};
    CHECK(SetEnvironmentVariableW(L"PATH", L"C:\\Windows\\System32"));
    require(generateConversion({conversionSource("x86"), {}, conversionOutput(), {}}, conversionAssets()));
    CHECK(require(readPackageManifest(conversionOutput())).architecture == Architecture::x86);
}
TEST("Conversion.ConfiguredAssets")
{
    CHECK(std::filesystem::equivalent(conversionSource(), std::filesystem::path(NYTERA_FIXTURE)));
}
TEST("Conversion.CancelPreservesOutput")
{
    auto output = conversionOutput(); Bytes before {std::byte{17}, std::byte{42}}; writeFile(output, before);
    bool reachedCommit = false;
    auto result = generateConversion({conversionSource(), {}, output, {}}, conversionAssets(), [&](double progress, const std::string&)
    {
        if (progress >= 0.9) { reachedCommit = true; return false; } return true;
    });
    CHECK(std::holds_alternative<Failure>(result) && reachedCommit);
    CHECK(readFile(output) == before);
}
TEST("Conversion.InvalidDestination")
{
    auto source = conversionOutput().parent_path() / "Source.dll";
    std::filesystem::copy_file(conversionSource(), source, std::filesystem::copy_options::overwrite_existing);
    auto before = sha256(readFile(source));
    CHECK(std::holds_alternative<Failure>(generateConversion({source, {}, source, {}}, conversionAssets())));
    CHECK(sha256(readFile(source)) == before);
    auto assets = conversionAssets();
    auto wrapper = conversionOutput().parent_path() / "Template.vst3";
    std::filesystem::copy_file(assets.effectTemplate, wrapper, std::filesystem::copy_options::overwrite_existing);
    assets.effectTemplate = wrapper; before = sha256(readFile(wrapper));
    CHECK(std::holds_alternative<Failure>(generateConversion({conversionSource(), {}, wrapper, {}}, assets)));
    CHECK(sha256(readFile(wrapper)) == before);
}
TEST("Conversion.SupportReexport")
{
    auto support = conversionOutput().parent_path() / "Support";
    std::filesystem::create_directories(support);
    writeFile(support / "Data.dat", Bytes{std::byte{42}});
    auto output = support / "Converted.vst3";
    ConversionRequest request {conversionSource(), support, output, {}};
    require(generateConversion(request, conversionAssets()));
    require(generateConversion(request, conversionAssets()));
    CHECK(require(readPackageManifest(output)).payloads.size() == 3);
}
TEST("Conversion.CancelInspection")
{
    auto source = conversionSource().parent_path() / "NyteraStalledFixture.dll";
    auto before = std::chrono::steady_clock::now();
    auto result = inspectSource(source, conversionAssets(), {}, [&]
    {
        return std::chrono::steady_clock::now() - before > std::chrono::milliseconds(100);
    });
    CHECK(std::holds_alternative<Failure>(result));
    CHECK(std::get<Failure>(result).message.find("cancelled") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
}
