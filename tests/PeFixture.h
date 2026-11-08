#pragma once
#include "common/Manifest.h"
#include "common/Codec.h"
#include <windows.h>
inline std::filesystem::path peFixture(nytera::Architecture architecture)
{
    auto bytes = nytera::readFile(NYTERA_FIXTURE);
    nytera::Reader header(bytes); header.raw(0x3c);
    auto machineOffset = header.number<uint32_t>() + 4;
    auto machine = architecture == nytera::Architecture::x86 ? 0x014c : 0x8664;
    // Exercise the pre-load machine guard with the built fixture's PE header.
    bytes[machineOffset] = static_cast<std::byte>(machine & 0xff);
    bytes[machineOffset + 1] = static_cast<std::byte>(machine >> 8);
    auto folder = std::filesystem::path(NYTERA_SOURCE_ROOT) / "out/pe-tests" / std::to_string(GetCurrentProcessId());
    std::filesystem::create_directories(folder);
    auto file = folder / (architecture == nytera::Architecture::x86 ? "x86.dll" : "x64.dll");
    nytera::writeFile(file, bytes); return file;
}
