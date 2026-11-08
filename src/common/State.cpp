#include "State.h"
#include "Codec.h"
#include <algorithm>
#include <cmath>
namespace nytera
{
void validateState(const StateEnvelope& state)
{
    if (state.version != 1 || (state.architecture != Architecture::x86 && state.architecture != Architecture::x64)
        || state.kind < StateKind::parameters || state.kind > StateKind::presetChunk || state.program >= 4096
        || state.programName.size() > 65536 || state.programName.find('\0') != std::string::npos
        || state.parameters.size() > 65536 || state.chunk.size() > maximumChunkBytes
        || (state.kind == StateKind::parameters && !state.chunk.empty()) || (state.kind != StateKind::parameters && state.chunk.empty()))
        throw std::runtime_error("Invalid state envelope fields");
    for (float value : state.parameters)
        if (!std::isfinite(value) || value < 0 || value > 1) throw std::runtime_error("Invalid normalized state parameter");
}
Bytes encodeState(const StateEnvelope& state)
{
    validateState(state);
    Writer writer;
    writer.bytes.reserve(128 + state.programName.size() + state.parameters.size() * sizeof(float) + state.chunk.size());
    writer.number<uint32_t>(0x3153594e); writer.number(state.version);
    writer.raw(state.conversionId); writer.raw(state.sourceHash); writer.number(state.sourceUniqueId);
    writer.number(static_cast<uint32_t>(state.architecture)); writer.number(static_cast<uint32_t>(state.kind));
    writer.number(state.program); writer.text(state.programName);
    writer.number(static_cast<uint32_t>(state.parameters.size()));
    for (float value : state.parameters) writer.floating(value);
    writer.number(static_cast<uint32_t>(state.chunk.size())); writer.raw(state.chunk);
    writer.raw(sha256(writer.bytes));
    if (writer.bytes.size() > maximumStateBytes) throw std::runtime_error("State envelope exceeds 256 MiB capacity");
    return std::move(writer.bytes);
}
Result<StateEnvelope> decodeState(std::span<const std::byte> bytes)
{
    try
    {
        if (bytes.size() < 32 || bytes.size() > maximumStateBytes) throw std::runtime_error("Invalid state envelope length");
        auto body = bytes.first(bytes.size() - 32);
        auto digest = sha256(body);
        if (!std::equal(digest.begin(), digest.end(), bytes.end() - 32)) throw std::runtime_error("State envelope checksum mismatch");
        Reader reader(body);
        if (reader.number<uint32_t>() != 0x3153594e) throw std::runtime_error("Invalid state envelope signature");
        StateEnvelope state;
        state.version = reader.number<uint32_t>();
        state.conversionId = reader.array<16>(); state.sourceHash = reader.array<32>(); state.sourceUniqueId = reader.number<uint32_t>();
        state.architecture = static_cast<Architecture>(reader.number<uint32_t>()); state.kind = static_cast<StateKind>(reader.number<uint32_t>());
        state.program = reader.number<uint32_t>(); state.programName = reader.text();
        auto count = reader.count(65536);
        for (uint32_t i = 0; i < count; ++i) state.parameters.push_back(reader.real32());
        auto chunk = reader.raw(reader.count(static_cast<uint32_t>(maximumChunkBytes)));
        state.chunk.assign(chunk.begin(), chunk.end());
        reader.finish();
        validateState(state);
        return state;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
Status validateStateBinding(const StateEnvelope& state, const Manifest& manifest)
{
    if (state.conversionId != manifest.conversionId || state.sourceHash != manifest.sourceHash
        || state.sourceUniqueId != manifest.sourceUniqueId || state.architecture != manifest.architecture
        || state.parameters.size() != manifest.parameters.size() || (manifest.schemaVersion >= 2 && state.program >= manifest.programs.names.size()))
        return Failure{ErrorCode::identity, "Saved state belongs to a different conversion or source topology"};
    return std::monostate{};
}
}
