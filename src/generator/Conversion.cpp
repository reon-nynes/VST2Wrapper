#include "Conversion.h"
#include "common/Native.h"
#include "common/Deadline.h"
#include "package/Package.h"
#include <bcrypt.h>
#include <cstring>
#include <sstream>
namespace nytera
{
RuntimeAssets RuntimeAssets::fromDirectory(const std::filesystem::path& root)
{
    return {root / "runtime/x86/NyteraBridge.exe", root / "runtime/x64/NyteraBridge.exe", root / "runtime/Effect.vst3", root / "runtime/Instrument.vst3"};
}
namespace
{
struct TemporaryDirectory
{
    std::filesystem::path path;
    TemporaryDirectory()
    {
        wchar_t root[MAX_PATH + 1] {};
        if (!GetTempPathW(MAX_PATH, root)) throw std::runtime_error("Cannot locate temporary directory");
        Id128 token;
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(token.data()), static_cast<ULONG>(token.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot create inspection identity");
        path = std::filesystem::path(root) / (L"Nytera-" + utf8Path(hex(token)).wstring());
        if (!CreateDirectoryW(path.c_str(), nullptr)) throw std::runtime_error("Cannot create inspection directory");
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::vector<std::string> importedDlls(const std::filesystem::path& source)
{
    auto bytes = readFile(source);
    auto u16 = [&](size_t offset)
    {
        uint16_t value;
        if (offset > bytes.size() || bytes.size() - offset < 2) throw std::runtime_error("Truncated PE import table");
        std::memcpy(&value, bytes.data() + offset, 2); return value;
    };
    auto u32 = [&](size_t offset)
    {
        uint32_t value;
        if (offset > bytes.size() || bytes.size() - offset < 4) throw std::runtime_error("Truncated PE import table");
        std::memcpy(&value, bytes.data() + offset, 4); return value;
    };
    auto pe = size_t(u32(0x3c)); auto sections = u16(pe + 6); auto optional = pe + 24;
    auto optionalSize = u16(pe + 20);
    if (sections > 96 || optionalSize < 112) throw std::runtime_error("Invalid PE sections");
    auto directory = optional + (u16(optional) == 0x20b ? 120 : 104);
    if (directory + 8 > optional + optionalSize) throw std::runtime_error("Missing PE import directory");
    auto offsetFor = [&](uint32_t rva) -> size_t
    {
        for (uint32_t index = 0; index < sections; ++index)
        {
            auto section = optional + optionalSize + index * 40;
            auto start = u32(section + 12), size = u32(section + 16), raw = u32(section + 20);
            if (rva >= start && uint64_t(rva) - start < size)
            {
                auto offset = uint64_t(raw) + rva - start;
                if (offset >= bytes.size()) break;
                return static_cast<size_t>(offset);
            }
        }
        throw std::runtime_error("PE import address lies outside file sections");
    };
    std::vector<std::string> imports;
    auto rva = u32(directory);
    if (!rva) return imports;
    auto descriptors = offsetFor(rva);
    for (size_t index = 0; index < 4096; ++index)
    {
        auto nameRva = u32(descriptors + index * 20 + 12);
        if (!nameRva) return imports;
        auto nameOffset = offsetFor(nameRva);
        std::string name;
        for (size_t length = 0; length < 512; ++length)
        {
            if (nameOffset + length >= bytes.size()) throw std::runtime_error("Truncated imported DLL name");
            auto character = static_cast<char>(bytes[nameOffset + length]);
            if (!character) break;
            name += character;
        }
        if (name.empty() || name.size() == 512) throw std::runtime_error("Invalid imported DLL name");
        imports.push_back(std::move(name));
    }
    throw std::runtime_error("Too many imported DLLs");
}
void stageSupport(const std::filesystem::path& support, const std::filesystem::path& destination, CancelCallback cancelled)
{
    if (GetFileAttributesW(support.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Choose a physical support directory");
    uint32_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(support))
    {
        if (cancelled && cancelled()) throw std::runtime_error("Inspection cancelled");
        if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Support files contain a reparse point");
        if (!entry.is_regular_file()) continue;
        if (++count > 64000) throw std::runtime_error("Too many support files");
        auto target = destination / relativePayloadPath(pathUtf8(entry.path().lexically_relative(support)));
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing);
    }
}
Status runInspector(const std::filesystem::path& source, const std::filesystem::path& helper,
    const std::filesystem::path& output, const std::filesystem::path& logPath, CancelCallback cancelled)
{
    SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle log(CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log.valid()) return Failure{ErrorCode::io, "Cannot create inspection log"};
    STARTUPINFOEXW startup {}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = startup.StartupInfo.hStdInput = log.get();
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> attributes(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) return Failure{ErrorCode::io, "Cannot prepare inspector launch"};
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup {startup.lpAttributeList};
    HANDLE inherited[] {log.get()};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        return Failure{ErrorCode::io, "Cannot restrict inspector handles"};
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.valid() || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Failure{ErrorCode::io, "Cannot create inspector job"};
    auto command = quoteArgument(std::filesystem::absolute(helper).wstring()) + L" --inspect " + quoteArgument(source.wstring()) + L" --output " + quoteArgument(output.wstring());
    PROCESS_INFORMATION information {};
    if (!CreateProcessW(std::filesystem::absolute(helper).c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, source.parent_path().c_str(), &startup.StartupInfo, &information))
        return Failure{ErrorCode::io, "Cannot launch matched inspector. Windows error " + std::to_string(GetLastError())};
    Handle process(information.hProcess), thread(information.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get()))
    { TerminateProcess(process.get(), 1); return Failure{ErrorCode::io, "Cannot own inspector process"}; }
    ResumeThread(thread.get());
    Deadline deadline(std::chrono::seconds(60));
    while (WaitForSingleObject(process.get(), 30) == WAIT_TIMEOUT)
    {
        if ((cancelled && cancelled()) || deadline.remaining().count() == 0)
        {
            TerminateJobObject(job.get(), 1); WaitForSingleObject(process.get(), 300);
            return Failure{ErrorCode::timeout, cancelled && cancelled() ? "Inspection cancelled" : "Source initialization exceeded 60 seconds"};
        }
    }
    DWORD code = 1; GetExitCodeProcess(process.get(), &code); log.reset();
    if (code != 0)
    {
        auto bytes = readFile(logPath); std::string detail(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return Failure{ErrorCode::plugin, "Inspector exit " + std::to_string(code) + "\n" + detail};
    }
    return std::monostate{};
}
}
Result<InspectionResult> inspectSource(const std::filesystem::path& requested, const RuntimeAssets& assets,
    const std::filesystem::path& support, CancelCallback cancelled)
{
    try
    {
        auto source = std::filesystem::absolute(requested);
        auto machine = inspectPeArchitecture(source);
        if (auto* failure = std::get_if<Failure>(&machine)) return *failure;
        auto architecture = std::get<Architecture>(machine);
        InspectionResult result; result.helper = assets.helper(architecture); result.imports = importedDlls(source);
        TemporaryDirectory temporary;
        auto inspectedSource = source;
        if (!support.empty())
        {
            auto staged = temporary.path / L"payload"; std::filesystem::create_directory(staged);
            stageSupport(std::filesystem::absolute(support), staged, cancelled);
            inspectedSource = staged / source.filename();
            std::filesystem::copy_file(source, inspectedSource, std::filesystem::copy_options::overwrite_existing);
        }
        auto output = temporary.path / L"inspection.manifest";
        auto run = runInspector(inspectedSource, result.helper, output, temporary.path / L"inspection.log", cancelled);
        if (auto* failure = std::get_if<Failure>(&run))
        {
            for (const auto& name : result.imports) failure->message += "\nImports " + name;
            failure->message += "\nCheck the source installation, activation and matching-architecture dependencies. Optional support files can be supplied and retested.";
            return *failure;
        }
        auto decoded = decodeManifest(readFile(output));
        if (auto* failure = std::get_if<Failure>(&decoded)) return *failure;
        result.manifest = std::get<Manifest>(std::move(decoded));
        if (result.manifest.architecture != architecture || result.manifest.sourceHash != sha256(readFile(source)))
            throw std::runtime_error("Source changed during inspection");
        const auto& info = result.manifest;
        std::ostringstream report;
        report << info.displayName << "\n" << (architecture == Architecture::x86 ? "32-bit" : "64-bit") << " source to 64-bit VST3\n"
            << "Vendor " << info.sourceVendor << "\n" << info.parameters.size() << " parameters, " << info.programs.names.size() << " programs\n"
            << info.inputs.size() << " inputs, " << info.outputs.size() << " outputs\nOriginal editor " << info.editorSize.width << " x " << info.editorSize.height
            << "\nSource latency " << info.initialDelay << " samples\nMIDI input " << (info.midiInput ? "yes" : "no") << ", output " << (info.midiOutput ? "yes" : "no")
            << "\n\nImported DLLs\n";
        for (const auto& name : result.imports) report << name << '\n';
        report << "\nPlugin activation, registry data and installed runtimes remain source-plugin prerequisites.";
        result.report = report.str(); return result;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::io, error.what()}; }
}
Status generateConversion(const ConversionRequest& request, const RuntimeAssets& assets, ProgressCallback progress)
{
    try
    {
        auto extension = request.output.extension().wstring();
        CharUpperBuffW(extension.data(), static_cast<DWORD>(extension.size()));
        if (extension != L".VST3") throw std::runtime_error("Choose a .vst3 output file");
        if (std::filesystem::exists(request.output))
            for (const auto& input : {request.source, assets.x86Helper, assets.x64Helper, assets.effectTemplate, assets.instrumentTemplate})
                if (std::filesystem::equivalent(request.output, input)) throw std::runtime_error("Output would replace a source DLL or application runtime asset");
        auto update = [&](double value, const std::string& message) { return !progress || progress(value, message); };
        if (!update(0.05, "Inspecting source DLL")) throw std::runtime_error("Conversion cancelled");
        auto inspected = request.inspected;
        if (!inspected)
        {
            auto result = inspectSource(request.source, assets, request.supportDirectory, [&] { return !update(0.1, "Waiting for matched inspector"); });
            if (auto* failure = std::get_if<Failure>(&result)) return *failure;
            inspected = std::get<InspectionResult>(std::move(result));
        }
        if (!update(0.4, "Embedding source DLL and matched runtime")) throw std::runtime_error("Conversion cancelled");
        const auto& info = inspected->manifest;
        auto wrapper = info.kind == PluginKind::instrument ? assets.instrumentTemplate : assets.effectTemplate;
        auto status = writePackage({info, wrapper, assets.helper(info.architecture), request.source, request.supportDirectory}, request.output,
            [&] { return !update(0.9, "Committing single-file VST3"); });
        if (auto* failure = std::get_if<Failure>(&status)) return *failure;
        update(1.0, "Generated " + pathUtf8(request.output)); return std::monostate{};
    }
    catch (const std::exception& error) { return Failure{ErrorCode::io, error.what()}; }
}
std::string outputFilename(const Manifest& manifest)
{
    auto name = manifest.displayName;
    for (char& character : name) if (static_cast<unsigned char>(character) < 32 || std::strchr("<>:\"/\\|?*", character)) character = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) name = "Converted Plugin";
    try { relativePayloadPath(name + ".vst3"); } catch (...) { name = "Nytera " + name; }
    return name + ".vst3";
}
}
