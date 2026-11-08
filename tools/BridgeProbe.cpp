#include "bridge/BridgeClient.h"
#include <iostream>
int wmain(int argc, wchar_t** argv)
{
    if (argc != 4) { std::cerr << "Usage: NyteraBridgeProbe <helper.exe> <plugin.dll> <manifest>\n"; return 2; }
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
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
