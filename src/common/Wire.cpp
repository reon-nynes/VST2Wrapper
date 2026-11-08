#include "Wire.h"
#include "Codec.h"
namespace nytera
{
static uint32_t payloadLimit(ControlCommand command)
{
    return command == ControlCommand::captureState || command == ControlCommand::restoreState ? 256u * 1024u * 1024u : 16u * 1024u * 1024u;
}
Bytes encodeFrame(const Frame& frame)
{
    if (frame.payload.size() > payloadLimit(frame.command)) throw std::runtime_error("Control payload too large");
    Writer writer;
    writer.number<uint32_t>(0x3142594e);
    writer.number<uint32_t>(1);
    writer.number(static_cast<uint32_t>(frame.command));
    writer.number(frame.requestId);
    writer.number(static_cast<uint32_t>(frame.payload.size()));
    writer.number<uint32_t>(frame.failed ? 1 : 0);
    writer.raw(frame.payload);
    return std::move(writer.bytes);
}
Result<uint32_t> framePayloadSize(std::span<const std::byte> data)
{
    try
    {
        Reader reader(data);
        if (reader.number<uint32_t>() != 0x3142594e || reader.number<uint32_t>() != 1)
            throw std::runtime_error("Incompatible bridge protocol");
        auto command = static_cast<ControlCommand>(reader.number<uint32_t>());
        if (command < ControlCommand::hello || command > ControlCommand::fixture)
            throw std::runtime_error("Unknown bridge command");
        reader.number<uint32_t>();
        auto size = reader.number<uint32_t>();
        if (size > payloadLimit(command) || reader.number<uint32_t>() > 1)
            throw std::runtime_error("Invalid bridge frame limits");
        reader.finish();
        return size;
    }
    catch (const std::exception& error) { return Failure{ErrorCode::invalidData, error.what()}; }
}
Result<Frame> decodeFrame(std::span<const std::byte> data)
{
    if (data.size() < 24) return Failure{ErrorCode::invalidData, "Truncated bridge header"};
    auto size = framePayloadSize(data.first(24));
    if (auto* failure = std::get_if<Failure>(&size)) return *failure;
    if (data.size() - 24 != std::get<uint32_t>(size)) return Failure{ErrorCode::invalidData, "Truncated bridge payload"};
    Reader reader(data);
    reader.raw(8);
    Frame frame;
    frame.command = static_cast<ControlCommand>(reader.number<uint32_t>());
    frame.requestId = reader.number<uint32_t>();
    reader.number<uint32_t>();
    frame.failed = reader.number<uint32_t>() == 1;
    frame.payload.assign(data.begin() + 24, data.end());
    return frame;
}
}
