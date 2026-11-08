#include "Vst2Instance.h"
#include "Server.h"
#include <iostream>
#include <objbase.h>
int wmain(int argc, wchar_t** argv)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    if (argc == 4 && std::wstring(argv[1]) == L"--serve")
    {
        result = nytera::runHelperServer(argv[2], nytera::pathUtf8(std::filesystem::path(argv[3])));
        CoUninitialize();
        return result;
    }
    if (argc == 5 && std::wstring(argv[1]) == L"--inspect" && std::wstring(argv[3]) == L"--output")
    {
        auto instance = nytera::Vst2Instance::open(argv[2]);
        if (auto* failure = std::get_if<nytera::Failure>(&instance)) std::cerr << failure->message << '\n';
        else
        {
            auto manifest = std::get<std::unique_ptr<nytera::Vst2Instance>>(instance)->inspect();
            if (auto* inspectionFailure = std::get_if<nytera::Failure>(&manifest)) std::cerr << inspectionFailure->message << '\n';
            else
            {
                try
                {
                    const auto& info = std::get<nytera::Manifest>(manifest);
                    nytera::writeFile(argv[4], nytera::encodeManifest(info));
                    std::cout << info.displayName << " | " << info.parameters.size() << " parameters | "
                              << info.inputs.size() << " in / " << info.outputs.size() << " out | editor "
                              << info.editorSize.width << "x" << info.editorSize.height << '\n';
                    result = 0;
                }
                catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
            }
        }
    }
    else std::cerr << "Usage: NyteraBridge --inspect <plugin.dll> --output <manifest>\n";
    CoUninitialize();
    return result;
}
