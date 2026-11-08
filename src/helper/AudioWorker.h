#pragma once
#include "Vst2Instance.h"
#include "bridge/AudioTransport.h"
#include <thread>
#include <mutex>
namespace nytera
{
class AudioWorker
{
public:
    AudioWorker(std::unique_ptr<SharedAudio>, Vst2Instance&, std::mutex&);
    ~AudioWorker();
    void fail() { InterlockedExchange(&memory->header().error, 1); }
    void acknowledge(const ParameterEvent& event) { memory->acknowledge(event); }
    void acknowledgeState(uint64_t sequence) { memory->acknowledgeState(sequence); }
private:
    void run(std::stop_token);
    std::unique_ptr<SharedAudio> memory;
    Vst2Instance& source;
    std::mutex& sourceMutex;
    std::vector<MidiEvent> midiViews;
    uint32_t lastGeneration = 0;
    std::jthread thread;
};
}
