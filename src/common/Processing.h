#pragma once
#include "Types.h"
namespace nytera
{
struct ProcessConfig
{
    double sampleRate = 44100;
    uint32_t maximumBlock = 512;
    uint32_t inputs = 2, outputs = 2;
    bool offline = false;
};
struct HostNotification
{
    uint32_t opcode = 0;
    int32_t index = 0;
    int64_t value = 0;
    float normalized = 0;
};
}
