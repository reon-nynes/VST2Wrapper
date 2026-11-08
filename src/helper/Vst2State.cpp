#include "Vst2Instance.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace nytera
{
Status Vst2Instance::openEditor(void* parent)
{
    std::vector<float> values;
    for (uint32_t index = 0; index < parameterValues.size(); ++index) values.push_back(effect->get_parameter(effect, index));
    auto program = dispatch(VST_EFFECT_OPCODE_PROGRAM_GET);
    auto* previous = applyingParameter; applyingParameter = this;
    try
    {
        dispatch(VST_EFFECT_OPCODE_EDITOR_OPEN, 0, 0, parent);
        dispatch(VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (effect->num_params != static_cast<int32_t>(values.size())) throw std::runtime_error("Editor initialization changed source parameter topology");
        if (effect->num_programs > 0 && dispatch(VST_EFFECT_OPCODE_PROGRAM_GET) != program)
        {
            dispatch(VST_EFFECT_OPCODE_PROGRAM_SET_BEGIN); dispatch(VST_EFFECT_OPCODE_PROGRAM_SET, 0, program); dispatch(VST_EFFECT_OPCODE_PROGRAM_SET_END);
        }
        for (uint32_t index = 0; index < values.size(); ++index)
            if (effect->get_parameter(effect, index) != values[index]) effect->set_parameter(effect, index, values[index]);
    }
    catch (const std::exception& error) { applyingParameter = previous; return Failure{ErrorCode::plugin, error.what()}; }
    applyingParameter = previous; return std::monostate{};
}
Result<ProgramInfo> Vst2Instance::programInfo()
{
    if (!effect || effect->num_programs < 0 || effect->num_programs > 4096)
        return Failure{ErrorCode::plugin, "Invalid source program count"};
    ProgramInfo info;
    auto count = static_cast<uint32_t>(std::max(1, effect->num_programs));
    info.current = static_cast<uint32_t>(std::clamp<intptr_t>(dispatch(VST_EFFECT_OPCODE_PROGRAM_GET), 0, count - 1));
    info.names.clear();
    for (uint32_t index = 0; index < count; ++index)
    {
        char name[1024] {};
        dispatch(29, 0, index, name); // effGetProgramNameIndexed
        if (index == info.current && !name[0]) dispatch(VST_EFFECT_OPCODE_PROGRAM_GET_NAME, 0, 0, name);
        name[sizeof(name) - 1] = 0;
        info.names.push_back(name[0] ? std::string(name) : "Program " + std::to_string(index + 1));
    }
    return info;
}
Status Vst2Instance::setProgram(uint32_t index, uint64_t sequence, bool mirror)
{
    if (!effect || index >= static_cast<uint32_t>(std::max(1, effect->num_programs)))
        return Failure{ErrorCode::invalidData, "Program index is out of range"};
    if (sequence && sequence <= programSequence) return std::monostate{};
    auto* previous = applyingParameter;
    applyingParameter = this;
    try
    {
        bool change = !mirror || dispatch(VST_EFFECT_OPCODE_PROGRAM_GET) != index;
        if (change && effect->num_programs > 0)
        {
            dispatch(VST_EFFECT_OPCODE_PROGRAM_SET_BEGIN);
            dispatch(VST_EFFECT_OPCODE_PROGRAM_SET, 0, index);
            dispatch(VST_EFFECT_OPCODE_PROGRAM_SET_END);
            resetEvents();
        }
        if (effect->num_params != static_cast<int32_t>(parameterValues.size())) throw std::runtime_error("Program selection changed source topology");
        for (uint32_t parameter = 0; parameter < parameterValues.size(); ++parameter)
        {
            if (sequence && parameterSequences[parameter] > sequence)
            {
                if (change) effect->set_parameter(effect, parameter, parameterValues[parameter]);
            }
            else
            {
                parameterSequences[parameter] = sequence;
                parameterValues[parameter] = effect->get_parameter(effect, parameter);
            }
            callbackParameterSequences[parameter] = parameterSequences[parameter];
        }
        programSequence = sequence;
    }
    catch (const std::exception& error) { applyingParameter = previous; return Failure{ErrorCode::plugin, error.what()}; }
    applyingParameter = previous;
    if (!mirror && applyingParameter != this && onNotification) onNotification({42, 0, index, 0});
    return std::monostate{};
}
Result<StateEnvelope> Vst2Instance::captureState()
{
    try
    {
        if (!binding)
        {
            auto inspected = inspect();
            if (auto* failure = std::get_if<Failure>(&inspected)) return *failure;
        }
        if (effect->num_params != static_cast<int32_t>(binding->parameters.size()))
            return Failure{ErrorCode::unsupported, "Source parameter topology changed"};
        auto programs = programInfo();
        if (auto* failure = std::get_if<Failure>(&programs)) return *failure;
        const auto& info = std::get<ProgramInfo>(programs);
        StateEnvelope state;
        state.conversionId = binding->conversionId; state.sourceHash = binding->sourceHash;
        state.sourceUniqueId = binding->sourceUniqueId; state.architecture = binding->architecture;
        state.program = info.current; state.programName = info.names[info.current].substr(0, 23);
        for (uint32_t index = 0; index < binding->parameters.size(); ++index)
        {
            float value = effect->get_parameter(effect, index);
            if (!std::isfinite(value) || value < 0 || value > 1) throw std::runtime_error("Source state has an invalid normalized parameter");
            state.parameters.push_back(value);
        }
        if (effect->flags & VST_EFFECT_FLAG_CHUNKS)
        {
            for (int32_t preset = 0; preset <= 1; ++preset)
            {
                void* data = nullptr;
                auto size = dispatch(VST_EFFECT_OPCODE_GET_CHUNK_DATA, preset, 0, &data);
                if (size < 0 || static_cast<uint64_t>(size) > maximumChunkBytes || (size > 0 && !data))
                    throw std::runtime_error("Invalid or oversized source state chunk");
                if (size == 0) continue;
                auto* first = static_cast<const std::byte*>(data);
                state.chunk.assign(first, first + size);
                state.kind = preset ? StateKind::presetChunk : StateKind::bankChunk;
                break;
            }
        }
        return state;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::plugin, error.what()}; }
}
Status Vst2Instance::restoreState(const StateEnvelope& state, uint64_t sequenceBarrier)
{
    try { validateState(state); }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
    if (!binding)
    {
        auto inspected = inspect();
        if (auto* failure = std::get_if<Failure>(&inspected)) return *failure;
    }
    auto valid = validateStateBinding(state, *binding);
    if (auto* failure = std::get_if<Failure>(&valid)) return *failure;
    if (state.kind != StateKind::parameters && !(effect->flags & VST_EFFECT_FLAG_CHUNKS))
        return Failure{ErrorCode::invalidData, "Source does not support the saved chunk format"};
    auto* previous = applyingParameter;
    applyingParameter = this;
    try
    {
        auto selectProgram = [&]
        {
            if (effect->num_programs <= 0) return;
            dispatch(VST_EFFECT_OPCODE_PROGRAM_SET_BEGIN);
            dispatch(VST_EFFECT_OPCODE_PROGRAM_SET, 0, state.program);
            dispatch(VST_EFFECT_OPCODE_PROGRAM_SET_END);
        };
        selectProgram();
        if (state.kind != StateKind::parameters)
        {
            if (state.chunk.empty() || state.chunk.size() > maximumChunkBytes) throw std::runtime_error("Invalid source chunk length");
            struct PatchInfo { int32_t version, id, pluginVersion, programs; char reserved[48]; };
            PatchInfo patch {1, effect->unique_id, effect->version, effect->num_programs, {}};
            dispatch(state.kind == StateKind::bankChunk ? 75 : 76, 0, 0, &patch);
            if (dispatch(VST_EFFECT_OPCODE_SET_CHUNK_DATA, state.kind == StateKind::presetChunk ? 1 : 0,
                static_cast<intptr_t>(state.chunk.size()), const_cast<std::byte*>(state.chunk.data())) < 0)
                throw std::runtime_error("Source rejected saved state chunk");
            if (state.kind == StateKind::bankChunk && dispatch(VST_EFFECT_OPCODE_PROGRAM_GET) != state.program) selectProgram();
        }
        else
        {
            if (!state.programName.empty())
            {
                auto name = state.programName.substr(0, 23);
                dispatch(VST_EFFECT_OPCODE_PROGRAM_SET_NAME, 0, 0, name.data());
            }
            for (uint32_t index = 0; index < state.parameters.size(); ++index) effect->set_parameter(effect, index, state.parameters[index]);
        }
        if (effect->num_params != static_cast<int32_t>(binding->parameters.size()) || effect->num_inputs != static_cast<int32_t>(binding->inputs.size())
            || effect->num_outputs != static_cast<int32_t>(binding->outputs.size()) || std::max(1, effect->num_programs) != static_cast<int32_t>(binding->programs.names.size()))
            throw std::runtime_error("State restore changed source topology");
        for (uint32_t index = 0; index < parameterValues.size(); ++index)
        {
            parameterSequences[index] = std::max(parameterSequences[index], sequenceBarrier);
            callbackParameterSequences[index] = parameterSequences[index];
            parameterValues[index] = effect->get_parameter(effect, index);
        }
        programSequence = std::max(programSequence, sequenceBarrier);
        resetEvents();
    }
    catch (const std::exception& error) { applyingParameter = previous; return Failure{ErrorCode::plugin, error.what()}; }
    applyingParameter = previous;
    return refreshLatency();
}
}
