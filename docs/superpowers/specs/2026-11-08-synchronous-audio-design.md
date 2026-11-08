# Synchronous audio bridge

The user approved synchronous processing on 2026-11-08 to remove the bridge's two-quantum delay, including x86 VST2 sources in an x64 VST3 host. Execution continues using the previously selected Native method.

## Approach

Each nonempty host callback publishes exactly its actual frame count to shared memory. The helper processes that block and signals completion. The wrapper returns that block's result in the same callback. Reported latency consists of the source plugin's delay alone. Source processing, communication and scheduling must fit the host's callback budget.

The previous asynchronous pipeline supplies scheduling headroom at the cost of two quanta of latency. Reducing its quantum still retains added latency. Synchronous IPC meets the approved zero-extra-sample-delay goal for both architectures while retaining helper isolation.

## Processing contract

- Accept every frame count from zero through the prepared maximum, including non-power-of-two maxima. Use the existing allocation stride for shared buffers and process only the submitted frames.
- Preserve sample-relative MIDI, transport and parameter offsets. Zero-frame events remain queued for the next nonempty callback. Keep source parameter/program sequence acknowledgements and state checkpoints.
- Preserve source latency reporting and compensated bypass. Keep float32 shared transport and float64 host conversion.
- Preserve source binding, embedded payloads, original editors, state persistence and retry.
- Version the shared-audio protocol because older helpers interpret frame counts differently. Previously generated VST3 files remain self-contained with their original helper. Regenerated packages use the updated transport and helper together.

## Failure handling

Wait on completion and the existing cancellation event. Allow 250 ms for a real-time block and 30 seconds for offline or prefetch work. These are failure bounds, independent of the audio device deadline. A timeout or malformed completion latches a transport failure and returns silence. Never reuse an in-flight slot after failure. The existing bridge watchdog and retry handle recovery. No heap allocation is permitted in prepared wrapper audio callbacks.

## Verification

Use x86 and x64 fixtures to prove zero bridge impulse displacement, source-only delay, variable/zero blocks, MIDI and parameter timing, bypass, float64 conversion and changing processing modes. Add direct transport timeout/cancellation regressions. Run Debug and Release suites and REAPER acceptance, then regenerate BBE and measure its reported delay and rendered alignment. LexHall remains excluded from compatibility testing at the user's request.
