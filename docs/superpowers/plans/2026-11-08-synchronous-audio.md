# Synchronous Audio Implementation Plan

> **For agentic workers** Use superpowers:executing-plans to implement this plan task-by-task. Native execution was selected earlier in this conversation.

**Goal** Remove bridge-added sample latency for x86 and x64 VST2 sources.

**Architecture** Publish one actual host block per shared-memory transaction and synchronously collect its result. Retain the existing source worker, sequence acknowledgements, failure watchdog, packaging and state envelope.

**Tech Stack** Windows C++20, JUCE 8.0.13, CMake, native tests and REAPER.

**Spec** docs/superpowers/specs/2026-11-08-synchronous-audio-design.md

## Global Constraints

- Zero bridge-added samples, with source latency preserved.
- Support x86 and x64 sources in the existing single-file x64 VST3 package.
- Prepared callbacks allocate no heap memory.
- Real-time waits terminate at 250 ms, offline/prefetch waits at 30 seconds, and stop interrupts either.
- Preserve existing saved states and source bindings.

## Review Focus

- Tiny or variable blocks and in-place buffers must return the current block without stale tails.
- Zero-frame MIDI/automation must survive until the next nonempty callback.
- Timeout and cancellation must leave worker-owned slots untouched and mute failed output.
- Source-only latency and bypass must stay correct through preparation, dynamic latency and retry.
- Old generated packages must keep working with their own embedded helper while new packages use the new protocol.

## Task 1 — Synchronous audio transaction

Modify `src/bridge/AudioTransport.h`, `src/bridge/AudioTransport.cpp` and `src/helper/AudioWorker.cpp`. Preserve the public `process` overloads, checkpoint and reset interfaces. `latency()` returns zero. Increment the shared layout version and validate actual nonzero frame counts against the prepared maximum. Replace quantum accumulation with one monotonically sequenced transaction per nonempty callback. Retain preallocated event storage, zero-block queuing, acknowledgements and cancellation.

- [x] Update `tests/AudioTests.cpp` to require delay-fixture impulse at sample 16 for x86/x64 and variable blocks, same-block MIDI/float64/time output and source-only bypass.
- [x] Add zero-latency, zero-frame-event, non-power-of-two maximum, timeout and cancellation regressions. Register them in `CMakeLists.txt`.
- [x] Build tests and observe zero-latency regressions fail with the existing pipeline.
- [x] Implement synchronous publication/completion and frame-count-aware helper processing.
- [x] Run `ctest --test-dir out/build/x64 -C Release -R '^Audio\.' --output-on-failure`. Expect all audio cases to pass.

## Task 2 — Host integration and compatibility

Modify timing assumptions in `tests/ProcessorTests.cpp`, `tests/ParameterTests.cpp` and `tests/RecoveryTests.cpp`. Keep source delay notifications and recovery semantics. Update `README.md` and `docs/testing/reaper-acceptance.md` to describe synchronous transport and measured results.

- [ ] Adjust legacy pipeline assertions to same-block output, including parameter changes, mode switches, saved-state retry and helper stop during rendering.
- [ ] Run `scripts/Build.ps1` and `scripts/Verify.ps1` for Release and Debug. Expect all x86 and x64 cases to pass.
- [ ] Run isolated REAPER audio/state/automation/recovery acceptance. Regenerate BBE with staged Release assets and verify source-only reported latency and original GUI.
- [ ] Request one fresh whole-change review, address important findings and rerun affected tests and the complete suite.
- [ ] Record verification and implementation decisions, commit the completed changes and provide the staged executable and regenerated BBE path.
