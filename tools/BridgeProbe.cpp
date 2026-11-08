#include "bridge/BridgeClient.h"
#include <iostream>
#include <windows.h>
#include <thread>
int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc != 4 && argc != 5) { std::cerr << "Usage: NyteraBridgeProbe <helper.exe> <plugin.dll> <manifest> [--editor-smoke]\n"; return 2; }
    try
    {
        auto decoded = nytera::decodeManifest(nytera::readFile(argv[3]));
        if (auto* failure = std::get_if<nytera::Failure>(&decoded)) throw std::runtime_error(failure->message);
        nytera::BridgeClient client;
        auto started = client.start(std::get<nytera::Manifest>(decoded), argv[1], argv[2]);
        if (auto* failure = std::get_if<nytera::Failure>(&started)) throw std::runtime_error(failure->message);
        auto result = client.request(nytera::ControlCommand::inspect);
        if (auto* failure = std::get_if<nytera::Failure>(&result)) throw std::runtime_error(failure->message);
        const auto& info = std::get<nytera::Manifest>(decoded);
        std::cout << info.displayName << ": helper process " << client.processId() << ", " << info.parameters.size() << " parameters\n";
        if (argc == 5 && std::wstring(argv[4]) == L"--editor-smoke")
        {
            auto caption = nytera::utf8Path(info.displayName + " - Nytera editor verification").wstring();
            RECT bounds {0, 0, static_cast<LONG>(info.editorSize.width), static_cast<LONG>(info.editorSize.height)};
            AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
            HWND parent = CreateWindowExW(0, L"STATIC", caption.c_str(), WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!parent) throw std::runtime_error("Cannot create diagnostic editor window");
            for (int pass = 0; pass < 2; ++pass)
            {
                auto opened = client.openEditor(reinterpret_cast<uint64_t>(parent));
                if (auto* failure = std::get_if<nytera::Failure>(&opened)) { DestroyWindow(parent); throw std::runtime_error(failure->message); }
                auto dimensions = client.editorSize();
                RECT frame {0, 0, static_cast<LONG>(dimensions.width), static_cast<LONG>(dimensions.height)};
                AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(parent));
                SetWindowPos(parent, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                ShowWindow(parent, SW_SHOWNORMAL);
                ShowWindow(parent, SW_SHOWNOACTIVATE);
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                while (IsWindow(parent) && std::chrono::steady_clock::now() < deadline)
                {
                    MSG message;
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                client.closeEditor();
            }
            DestroyWindow(parent);
            std::cout << "PASS original editor open/close/reopen\n";
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
