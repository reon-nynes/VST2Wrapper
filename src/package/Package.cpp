#include "Package.h"
#include <stdexcept>
#include <algorithm>
#include <set>
namespace nytera
{
std::filesystem::path relativePayloadPath(const std::string& text)
{
    if (text.empty() || text.find(':') != std::string::npos || text.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid payload filename");
    auto path = utf8Path(text);
    if (path.has_root_path()) throw std::runtime_error("Absolute payload paths are forbidden");
    for (const auto& segment : path)
    {
        auto name = segment.wstring();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' '
            || name.find_first_of(L"<>\"|?*") != std::wstring::npos)
            throw std::runtime_error("Invalid relative payload path");
        CharUpperBuffW(name.data(), static_cast<DWORD>(name.size()));
        auto base = name.substr(0, name.find(L'.'));
        if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL"
            || (base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") && base[3] >= L'1' && base[3] <= L'9')
            || name == L".NYTERA.COMPLETE" || name == L".NYTERA.LEASE")
            throw std::runtime_error("Reserved payload filename");
    }
    return path;
}
Status writePackage(const PackageInput& input, const std::filesystem::path& output, std::function<bool()> cancelled)
{
    std::filesystem::path temporary;
    HANDLE update = nullptr;
    try
    {
        Manifest manifest = input.manifest;
        std::vector<std::pair<PayloadEntry, Bytes>> payloads;
        std::set<std::wstring> names;
        auto addPayload = [&](const std::filesystem::path& source, const std::string& name, uint32_t id)
        {
            auto relative = relativePayloadPath(name);
            auto key = relative.wstring();
            CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
            if (!names.insert(key).second) throw std::runtime_error("Duplicate payload destination");
            auto bytes = readFile(source);
            PayloadEntry entry {pathUtf8(relative), sha256(bytes), bytes.size(), id};
            payloads.emplace_back(std::move(entry), std::move(bytes));
        };
        if (!input.source.empty() || !input.helper.empty())
        {
            if (input.source.empty() || input.helper.empty()) throw std::runtime_error("Source DLL and helper must be supplied together");
            auto sourceArchitecture = inspectPeArchitecture(input.source);
            auto helperArchitecture = inspectPeArchitecture(input.helper);
            if (!std::holds_alternative<Architecture>(sourceArchitecture) || !std::holds_alternative<Architecture>(helperArchitecture)
                || std::get<Architecture>(sourceArchitecture) != manifest.architecture || std::get<Architecture>(helperArchitecture) != manifest.architecture)
                throw std::runtime_error("Source or helper architecture mismatch");
            if (utf8Path(manifest.dllName).filename() != utf8Path(manifest.dllName)) throw std::runtime_error("Source DLL must have a filename binding");
            addPayload(input.helper, "NyteraBridge.exe", 102);
            addPayload(input.source, manifest.dllName, 103);
            if (payloads.back().first.hash != manifest.sourceHash) throw std::runtime_error("Source DLL changed after inspection");
            uint32_t resourceId = 1000;
            if (!input.supportDirectory.empty())
            {
                auto support = std::filesystem::canonical(input.supportDirectory);
                if (GetFileAttributesW(support.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Reparse support directories require a physical source directory");
                std::vector<std::filesystem::path> files;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(support))
                {
                    if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Reparse support entries are unsupported");
                    if (entry.is_regular_file() && !std::filesystem::equivalent(entry.path(), input.source)
                        && !(std::filesystem::exists(output) && std::filesystem::equivalent(entry.path(), output))) files.push_back(entry.path());
                }
                std::sort(files.begin(), files.end());
                for (const auto& file : files)
                {
                    if (resourceId > 65000) throw std::runtime_error("Too many support files");
                    addPayload(file, pathUtf8(file.lexically_relative(support)), resourceId++);
                }
            }
            manifest.payloads.clear();
            for (const auto& payload : payloads) manifest.payloads.push_back(payload.first);
        }
        for (const auto& entry : manifest.payloads) relativePayloadPath(entry.path);
        auto directory = std::filesystem::absolute(output).parent_path();
        std::filesystem::create_directories(directory);
        wchar_t filename[MAX_PATH];
        if (!GetTempFileNameW(directory.c_str(), L"nyt", 0, filename))
            throw std::runtime_error("Cannot create temporary output");
        temporary = filename;
        std::filesystem::copy_file(input.wrapperTemplate, temporary, std::filesystem::copy_options::overwrite_existing);
        update = BeginUpdateResourceW(temporary.c_str(), FALSE);
        if (!update) throw std::runtime_error("Cannot open template resources");
        for (auto& [entry, data] : payloads)
            if (!UpdateResourceW(update, RT_RCDATA, MAKEINTRESOURCEW(entry.resourceId), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), data.data(), static_cast<DWORD>(data.size())))
                throw std::runtime_error("Cannot embed runtime payload");
        auto bytes = encodeManifest(manifest);
        if (!UpdateResourceW(update, RT_RCDATA, MAKEINTRESOURCEW(101), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), bytes.data(), static_cast<DWORD>(bytes.size())))
            throw std::runtime_error("Cannot embed binding manifest");
        HANDLE completed = update;
        update = nullptr;
        if (!EndUpdateResourceW(completed, FALSE)) throw std::runtime_error("Cannot finish output resources");
        if (cancelled && cancelled()) throw std::runtime_error("Conversion cancelled before export commit");
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
