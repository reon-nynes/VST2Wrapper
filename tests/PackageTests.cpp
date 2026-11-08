#include "package/Cache.h"
#include "helper/Vst2Instance.h"
#include "bridge/BridgeClient.h"
#include "Test.h"
#include <future>
#include <winioctl.h>
#include "common/Codec.h"
using namespace nytera;
struct PackageFixture
{
    std::filesystem::path root, output, source;
    Manifest manifest;
    PackageInput input;
    HMODULE module = nullptr;
    explicit PackageFixture(const std::string& name)
    {
        root = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/package-tests" / (name + "-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        source = root / "Embedded.dll";
        std::filesystem::copy_file(NYTERA_FIXTURE, source, std::filesystem::copy_options::overwrite_existing);
        auto instance = require(Vst2Instance::open(source));
        manifest = require(instance->inspect());
        auto helper = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build/x64/" NYTERA_TEST_CONFIG "/NyteraBridge.exe";
        input = {manifest, NYTERA_EFFECT_TEMPLATE, helper, source, {}};
        output = root / "Embedded.vst3";
    }
    void package()
    {
        require(writePackage(input, output));
        manifest = require(readPackageManifest(output));
        module = LoadLibraryExW(output.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        CHECK(module);
    }
    ~PackageFixture() { if (module) FreeLibrary(module); }
};
TEST("Package.Relocate")
{
    PackageFixture fixture("relocate");
    fixture.package();
    CHECK(fixture.manifest.payloads.size() == 2);
    std::filesystem::remove(fixture.source);
    auto payload = require(ensurePayload(fixture.module, fixture.manifest, fixture.root / "cache"));
    CHECK(sha256(readFile(payload.source)) == fixture.manifest.sourceHash);
    BridgeClient client;
    require(client.start(fixture.manifest, payload.helper, payload.source));
    CHECK(require(decodeManifest(require(client.request(ControlCommand::inspect)))).displayName == "Nytera Fixture");
}
TEST("Package.ConcurrentExtract")
{
    PackageFixture fixture("concurrent"); fixture.package();
    auto extract = [&] { return ensurePayload(fixture.module, fixture.manifest, fixture.root / "cache"); };
    auto a = std::async(std::launch::async, extract);
    auto b = std::async(std::launch::async, extract);
    auto first = require(a.get()); auto second = require(b.get());
    CHECK(first.source == second.source);
    CHECK(first.lease.valid() && second.lease.valid());
    CHECK(sha256(readFile(first.source)) == fixture.manifest.sourceHash);
}
TEST("Package.CorruptCache")
{
    PackageFixture fixture("corrupt"); fixture.package();
    std::filesystem::path cached;
    {
        auto payload = require(ensurePayload(fixture.module, fixture.manifest, fixture.root / "cache"));
        cached = payload.source;
    }
    writeFile(cached, Bytes{std::byte{1}, std::byte{2}});
    auto repaired = require(ensurePayload(fixture.module, fixture.manifest, fixture.root / "cache"));
    CHECK(sha256(readFile(repaired.source)) == fixture.manifest.sourceHash);
}
TEST("Package.UnicodePaths")
{
    PackageFixture fixture("unicode");
    fixture.output = fixture.root / utf8Path("日本語 Wrapped plugin.vst3");
    fixture.package();
    auto payload = require(ensurePayload(fixture.module, fixture.manifest, fixture.root / utf8Path("日本語 old plugin cache")));
    CHECK(std::filesystem::exists(payload.source));
    CHECK(sha256(readFile(payload.source)) == fixture.manifest.sourceHash);
    BridgeClient client;
    require(client.start(fixture.manifest, payload.helper, payload.source));
}
TEST("Package.PathEscape")
{
    PackageFixture fixture("escape");
    fixture.input.source.clear(); fixture.input.helper.clear();
    for (const auto& path : {"../escaped.dll", "C:/escaped.dll", "CON", "x/file.dll:stream"})
    {
        fixture.input.manifest.payloads = {{path, {}, 1, 1000}};
        CHECK(std::holds_alternative<Failure>(writePackage(fixture.input, fixture.output)));
    }
}
TEST("Package.AtomicExport")
{
    PackageFixture fixture("atomic");
    require(writePackage(fixture.input, fixture.output));
    auto before = readFile(fixture.output);
    fixture.input.helper = fixture.root / "missing.exe";
    CHECK(std::holds_alternative<Failure>(writePackage(fixture.input, fixture.output)));
    CHECK(readFile(fixture.output) == before);
}
TEST("Package.ReparseDestination")
{
    PackageFixture fixture("reparse"); fixture.package();
    auto target = fixture.root / "outside";
    auto link = fixture.root / "junction";
    std::filesystem::create_directory(target);
    std::filesystem::create_directory(link);
    auto substitute = L"\\??\\" + std::filesystem::absolute(target).wstring();
    auto printable = std::filesystem::absolute(target).wstring();
    Writer data;
    auto substituteBytes = static_cast<uint16_t>(substitute.size() * sizeof(wchar_t));
    auto printableBytes = static_cast<uint16_t>(printable.size() * sizeof(wchar_t));
    data.number<uint32_t>(IO_REPARSE_TAG_MOUNT_POINT);
    data.number<uint16_t>(8 + substituteBytes + printableBytes + 4);
    data.number<uint16_t>(0);
    data.number<uint16_t>(0); data.number(substituteBytes);
    data.number<uint16_t>(substituteBytes + 2); data.number(printableBytes);
    data.raw(std::as_bytes(std::span(substitute))); data.number<uint16_t>(0);
    data.raw(std::as_bytes(std::span(printable))); data.number<uint16_t>(0);
    Handle directory(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    CHECK(directory.valid());
    DWORD returned = 0;
    CHECK(DeviceIoControl(directory.get(), FSCTL_SET_REPARSE_POINT, data.bytes.data(), static_cast<DWORD>(data.bytes.size()), nullptr, 0, &returned, nullptr));
    directory.reset();
    CHECK(std::holds_alternative<Failure>(ensurePayload(fixture.module, fixture.manifest, link)));
    CHECK(std::filesystem::is_empty(target));
    CHECK(RemoveDirectoryW(link.c_str()));
}
