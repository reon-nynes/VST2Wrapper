#include "helper/EditorHost.h"
#include "bridge/BridgeClient.h"
#include "Test.h"
using namespace nytera;
struct ParentWindow
{
    HWND handle = CreateWindowExW(0, L"STATIC", L"Nytera editor test", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~ParentWindow() { if (handle) DestroyWindow(handle); }
};
TEST("Editor.Reopen")
{
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    ParentWindow parent;
    EditorHost editor(*instance);
    require(editor.open(parent.handle));
    auto child = editor.window();
    CHECK(IsWindow(child));
    CHECK(GetParent(child) == parent.handle);
    CHECK(GetWindow(child, GW_CHILD));
    editor.close();
    CHECK(!IsWindow(child));
    require(editor.open(parent.handle));
    CHECK(GetWindow(editor.window(), GW_CHILD));
}
TEST("Editor.Resize")
{
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    ParentWindow parent;
    EditorHost editor(*instance);
    instance->onNotification = [&](const HostNotification& notification)
    {
        if (notification.opcode == VST_HOST_OPCODE_EDITOR_RESIZE)
            editor.resize({static_cast<uint32_t>(notification.index), static_cast<uint32_t>(notification.value)});
    };
    require(editor.open(parent.handle));
    SendMessageW(GetWindow(editor.window(), GW_CHILD), WM_COMMAND, 102, 0);
    CHECK(editor.size() == (Size{400, 200}));
    RECT rectangle;
    GetClientRect(editor.window(), &rectangle);
    CHECK(rectangle.right == 400 && rectangle.bottom == 200);
}
TEST("Editor.OwnerTeardown")
{
    auto instance = require(Vst2Instance::open(NYTERA_FIXTURE));
    ParentWindow parent;
    EditorHost editor(*instance);
    CHECK(std::holds_alternative<Failure>(editor.open(nullptr)));
    require(editor.open(parent.handle));
    DestroyWindow(parent.handle);
    parent.handle = nullptr;
    editor.close(); editor.close();
    CHECK(!editor.window());
}
#if defined(_WIN64)
TEST("Editor.CrossBitness")
{
    for (const auto& architecture : {std::string("x86"), std::string("x64")})
    {
        auto folder = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/build" / architecture / NYTERA_TEST_CONFIG;
        auto source = folder / "NyteraFixture.dll";
        Manifest manifest;
        manifest.sourceHash = sha256(readFile(source));
        manifest.sourceUniqueId = 0x4e795431;
        manifest.architecture = architecture == "x86" ? Architecture::x86 : Architecture::x64;
        manifest.parameters = {{0, 0, "Gain", "", 0.5f, true}};
        manifest.inputs = {{"Input 1"}, {"Input 2"}};
        manifest.outputs = {{"Output 1"}, {"Output 2"}};
        manifest = assignBindingIds(manifest);
        BridgeClient client;
        require(client.start(manifest, folder / "NyteraBridge.exe", source));
        ParentWindow parent;
        require(client.openEditor(reinterpret_cast<uint64_t>(parent.handle)));
        HWND container = GetWindow(parent.handle, GW_CHILD);
        CHECK(container);
        DWORD owner = 0;
        GetWindowThreadProcessId(container, &owner);
        CHECK(owner == client.processId());
        CHECK(GetWindow(container, GW_CHILD));
        client.closeEditor();
        CHECK(GetWindow(parent.handle, GW_CHILD) == nullptr);
    }
}
#endif
