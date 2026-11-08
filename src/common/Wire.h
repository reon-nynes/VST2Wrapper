#pragma once
#include "Types.h"
namespace nytera
{
enum class ControlCommand : uint32_t
{
    hello = 1, inspect, poll, shutdown, openEditor, closeEditor, prepare,
    captureState, restoreState, connectAudio, parameter, display, fixture, releaseAudio, programs
};
struct Frame
{
    ControlCommand command = ControlCommand::hello;
    uint32_t requestId = 0;
    bool failed = false;
    Bytes payload;
};
Bytes encodeFrame(const Frame&);
Result<Frame> decodeFrame(std::span<const std::byte>);
Result<uint32_t> framePayloadSize(std::span<const std::byte> header);
}
