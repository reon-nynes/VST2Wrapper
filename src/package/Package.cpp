#include "Package.h"
#include <stdexcept>
namespace nytera
{
Status writePackage(const PackageInput& input, const std::filesystem::path& output)
{
    std::filesystem::path temporary;
    HANDLE update = nullptr;
    try
    {
        auto directory = std::filesystem::absolute(output).parent_path();
        std::filesystem::create_directories(directory);
        wchar_t filename[MAX_PATH];
        if (!GetTempFileNameW(directory.c_str(), L"nyt", 0, filename))
            throw std::runtime_error("Cannot create temporary output");
        temporary = filename;
        std::filesystem::copy_file(input.wrapperTemplate, temporary, std::filesystem::copy_options::overwrite_existing);
        update = BeginUpdateResourceW(temporary.c_str(), FALSE);
        if (!update) throw std::runtime_error("Cannot open template resources");
        auto bytes = encodeManifest(input.manifest);
        if (!UpdateResourceW(update, RT_RCDATA, MAKEINTRESOURCEW(101), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), bytes.data(), static_cast<DWORD>(bytes.size())))
            throw std::runtime_error("Cannot embed binding manifest");
        HANDLE completed = update;
        update = nullptr;
        if (!EndUpdateResourceW(completed, FALSE)) throw std::runtime_error("Cannot finish output resources");
        if (!MoveFileExW(temporary.c_str(), std::filesystem::absolute(output).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot publish output file (it may be in use)");
        return std::monostate{};
    }
    catch (const std::exception& error)
    {
        if (update) EndUpdateResourceW(update, TRUE);
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        return Failure{ErrorCode::io, error.what()};
    }
}
Bytes readResource(HMODULE module, uint32_t id)
{
    if (id > 65535) throw std::runtime_error("Invalid resource ID");
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) throw std::runtime_error("Missing embedded resource " + std::to_string(id));
    auto size = SizeofResource(module, resource);
    if (size > 1024u * 1024u * 1024u) throw std::runtime_error("Embedded resource exceeds payload limit");
    HGLOBAL loaded = LoadResource(module, resource);
    auto* data = static_cast<const std::byte*>(LockResource(loaded));
    if (!data) throw std::runtime_error("Cannot read embedded resource");
    return Bytes(data, data + size);
}
Result<Manifest> readModuleManifest(HMODULE module)
{
    try { return decodeManifest(readResource(module, 101)); }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
Result<Manifest> readPackageManifest(const std::filesystem::path& path)
{
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return Failure{ErrorCode::io, "Cannot read package " + pathUtf8(path)};
    auto manifest = readModuleManifest(module);
    FreeLibrary(module);
    return manifest;
}
}
