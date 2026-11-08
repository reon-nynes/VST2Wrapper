#pragma once
#include "Manifest.h"
namespace nytera
{
enum class StateKind : uint32_t { parameters = 0, bankChunk = 1, presetChunk = 2 };
constexpr size_t maximumStateBytes = 256u * 1024u * 1024u;
constexpr size_t maximumChunkBytes = maximumStateBytes - 1024u * 1024u;
struct StateEnvelope
{
    uint32_t version = 1;
    Id128 conversionId {};
    Hash256 sourceHash {};
    uint32_t sourceUniqueId = 0;
    Architecture architecture = Architecture::x64;
    StateKind kind = StateKind::parameters;
    uint32_t program = 0;
    std::string programName;
    std::vector<float> parameters;
    Bytes chunk;
    bool operator==(const StateEnvelope&) const = default;
};
Bytes encodeState(const StateEnvelope&);
void validateState(const StateEnvelope&);
Result<StateEnvelope> decodeState(std::span<const std::byte>);
Status validateStateBinding(const StateEnvelope&, const Manifest&);
}
