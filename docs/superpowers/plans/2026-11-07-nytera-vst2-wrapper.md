# Nytera VST2 Wrapper Implementation Plan

> **For agentic workers** REQUIRED SUB-SKILL. The user selected Native execution through superpowers:executing-plans. Implement this plan task by task. Steps use checkbox syntax for tracking.

**Goal.** Build a standalone Nytera application that turns an x86 or x64 Windows VST2 DLL into a dedicated single-file x64 VST3 for REAPER, embedding the source and bridge and presenting the original editor.

**Architecture.** Prebuilt JUCE VST3 templates obtain their identity and parameter layout from embedded binding metadata. A native architecture-matched helper owns each source plugin instance and editor. Shared memory transports audio and events, while a named-pipe control channel handles startup, editor operations, state, and diagnostics.

**Tech stack.** C++20, CMake 3.25+, MSVC v143, JUCE 8.0.13, Xaymar VST2 SDK, Win32, and CTest.

**Spec.** [Approved design](../specs/2026-11-07-nytera-vst2-wrapper-design.md).

## Global constraints

- Nytera is the company name.
- REAPER x64 is the first target.
- Both x86 and x64 VST2 DLLs are supported.
- Each output is a single physical `.vst3` file with its VST2 DLL and bridge runtime embedded.
- The generated plugin presents the original VST2 editor inside REAPER's plugin window.
- Each generated file has an immutable source-plugin binding.
- Initial sources are effects and instruments with editors and fixed parameter/channel topology.
- Shell DLLs receive an explicit unsupported diagnostic in this version.
- Export uses prebuilt templates. End users require the released application and its assets.
- Keep the supplied DLLs as local fixtures and exclude them from source-control history and distributed packages.
- Read both this plan and the approved design before implementing a task.

## Review focus

These five boundary cases receive explicit checks in the tasks owning them.

1. A REAPER scan constructs a processor with the editor closed. Identity and the full parameter list must already be available. Task 2 owns this check.
2. Paths contain Unicode, spaces, traversal components, or Windows reparse points. Genuine source paths work, while packaged paths stay within the extraction root. Task 6 owns these checks.
3. Two instances extract the same conversion while an earlier helper uses its files. Both load complete payloads and active files remain usable. Task 6 owns this check.
4. Project state arrives before startup or a helper dies during snapshot capture. Restore ordering is deterministic and the last complete snapshot survives. Tasks 9 and 10 own these checks.
5. Host blocks vary in size, processing includes zero-length blocks, and transport seeks while events are queued. Audio, MIDI, transport, and reported latency remain aligned. Task 7 owns this check.

## Workspace and build decisions

The current workspace contains the approved design and four DLLs under `vst2demo`. It has no Git repository. CMake 4.4.5, Git, Ninja, and Visual Studio Build Tools 2022 with the x86/x64 compiler component were found. REAPER was absent from PATH and the two checked default installation locations. Locate an existing installation before arranging a portable test installation. Keep the test profile and generated plugins under `out/reaper-test` so acceptance work uses a dedicated profile.

Pin JUCE to commit `7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2`, tag `8.0.13`, and Xaymar SDK to `339d4f31590bf77c0d0d248e09a380ac6285e069`. These revisions were verified with `git ls-remote` during planning. Fetch them in build directories and retain their license notices. Use JUCE's included VST3 SDK for the VST3 side and Xaymar's headers for the VST2 side.

Use Visual Studio 17 2022 presets with `architecture` set to `x64` and `Win32`, toolset `v143`, and separate binary directories `out/build/x64` and `out/build/x86`. Presets `x64-debug`, `x64-release`, `x86-debug`, and `x86-release` configure their corresponding architecture and configuration. The x86 configuration builds native helpers, common code, and controlled fixtures. Link the static MSVC runtime consistently inside each executable or DLL. Memory returned by a source plugin is copied before leaving its owning process.

Use one plan because the generator, factory identity, embedded payload, bridge, and project state share the same binding contract. Their intermediate deliverables are testable, and their final acceptance requires the complete path.

## File and interface map

All source paths below are relative to `D:/Dev/VST2Wrapper`. Files sharing a stem include the `.h` and `.cpp` pair unless marked otherwise.

| Path | Responsibility |
| --- | --- |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/Dependencies.cmake` | Targets, architecture separation, pinned dependencies |
| `cmake/PatchJuce.cmake`, `patches/juce-8.0.13-nytera-vst3.patch` | Checked adaptation of JUCE factory and parameter identity |
| `src/common/Types.h`, `Manifest.*`, `Wire.*`, `State.*` | Portable binding, wire, and state formats |
| `src/package/Package.*`, `Cache.*` | PE resources, payload validation, atomic extraction |
| `src/helper/Main.cpp`, `Vst2Instance.*`, `EditorHost.*` | Native VST2 lifecycle and original editor |
| `src/bridge/Control.*`, `BridgeClient.*`, `AudioTransport.*` | Named pipes, helper process ownership, shared audio transport |
| `src/plugin/ModuleBinding.*`, `Processor.*`, `Parameter.*`, `Editor.*` | Metadata-backed JUCE plugin and host-facing behavior |
| `src/generator/Main.cpp`, `MainComponent.*`, `Conversion.*` | Standalone conversion workflow |
| `tests/TestMain.cpp`, `tests/*Tests.cpp`, `tests/fixtures/FixtureVst2.cpp` | Targeted ABI, identity, timing, state, and recovery regressions |
| `tools/FactoryProbe.cpp`, `tools/BridgeProbe.cpp` | Diagnostic executable entry points used by CTest and feasibility gates |
| `scripts/Build.ps1`, `scripts/Verify.ps1` | Reproducible architecture builds and acceptance checks |
| `docs/testing/reaper-acceptance.md`, `README.md` | Results matrix and necessary build/use instructions |

### Shared contracts

Define these in namespace `nytera`. Use native C++ types inside a process and explicit codecs at every boundary.

- `Bytes = std::vector<std::byte>`, `Id128 = std::array<std::byte, 16>`, `Hash256 = std::array<std::byte, 32>`.
- `Failure { ErrorCode code; std::string message; }`, `template<class T> using Result = std::variant<T, Failure>`, `Status = Result<std::monostate>`.
- `Architecture { x86, x64 }`, `PluginKind { effect, instrument }`, and `Size { uint32_t width, height; }`.
- `ParameterInfo` contains original index, persisted VST3 numeric ID, name, label, initial normalized value, and automatable flag.
- `PayloadEntry` contains a relative UTF-8 path, hash, byte length, and resource ID.
- `Manifest` contains schema version, conversion/processor/controller IDs, source DLL hash and original unique ID, architecture, kind, DLL filename, display name, source vendor, parameter/pin descriptors, MIDI capabilities, editor size, initial latency, tail information, and payload entries.
- `PackageInput` contains a `Manifest`, selected wrapper-template path, helper path, DLL path, and optional support-directory path.
- `ProcessConfig` contains sample rate, maximum host block size, active input/output channels, and offline flag. `TransportSnapshot` contains project sample position and validity-tagged timing fields.
- `ParameterEvent` contains parameter index, normalized value, absolute processing sample position, and origin/gesture flags. `MidiEvent` contains absolute processing sample position and owned message bytes outside the audio callback.
- `StateEnvelope` contains format version, conversion ID, source hash/unique ID, encoding, current program, and a complete state payload.

The binding uses a versioned little-endian binary manifest. UTF-8 strings carry explicit lengths. Native struct layout is never the serialized layout. Derive IDs with SHA-256 and distinct namespace prefixes from source DLL content hash, source unique ID, and architecture. Keep a source-derived identity stable across filename changes and compatible wrapper updates. Derive parameter IDs from binding identity and original index, constrain them to `0x10000000..0x1fffffff`, and persist deterministic collision resolution. Keep JUCE bypass, program, and MIDI-controller IDs disjoint.

Common targeted test conventions use a lightweight `CHECK` runner in `tests/TestMain.cpp`. Each named case registers with CTest. Test commands below use `ctest --test-dir out/build/x64 -C Debug -R <case> --output-on-failure`. Cross-architecture tests receive helper/fixture paths from staged build assets. Tests are concentrated on the specific ABI, ordering, concurrency, and identity regressions that manual commercial-plugin checks would miss.

## Task 1. Establish reproducible builds and the binding codec

**Files**

Create the root CMake files, `.gitignore`, `cmake/Dependencies.cmake`, `src/common/Types.h`, `src/common/Manifest.*`, `tests/TestMain.cpp`, and `tests/ManifestTests.cpp`.

**Interfaces**

Produces `Result<Architecture> inspectPeArchitecture(const std::filesystem::path&)`, `Bytes encodeManifest(const Manifest&)`, `Result<Manifest> decodeManifest(std::span<const std::byte>)`, and `Manifest assignBindingIds(Manifest)`.

- [x] Establish Git history for the existing approved documents. Ignore `out/`, downloaded dependencies, local test profiles, and `vst2demo/` before staging files.
- [x] Configure architecture-separated targets and a test runner, with failing cases `Manifest.RoundTrip`, `Manifest.Truncated`, `Manifest.Identity`, and `Manifest.PeArchitecture`.
- [x] Assert `decodeManifest(encodeManifest(m))` preserves every field, truncated/count-overflow input fails, renamed identical DLL bytes keep IDs, changed source bytes change IDs, and all supplied PE architectures match the design table.
- [x] Run `cmake --preset x64-debug` and build `NyteraTests`. Run `ctest --test-dir out/build/x64 -C Debug -R Manifest --output-on-failure` and confirm the missing behavior fails.
- [x] Implement the four interfaces with bounded decoding, validated PE offsets, and Windows SHA-256 support. Keep common code free of JUCE dependencies so it builds for x86.
- [x] Build and run the same codec checks in both architectures. Expected result is passing round-trip, malformed-input, identity, and architecture cases.
- [x] Commit this independently testable build and codec deliverable.

## Task 2. Prove single-file VST3 identity in REAPER

**Files**

Create `cmake/PatchJuce.cmake`, the JUCE patch, `src/plugin/ModuleBinding.*`, `Processor.*`, initial `Parameter.*`, `src/package/Package.*`, `tools/FactoryProbe.cpp`, and `tests/FactoryTests.cpp`. Modify root target definitions.

**Interfaces**

Consumes Task 1's manifest codec. Produces `const Manifest& moduleBinding()`, `Status writePackage(const PackageInput&, const std::filesystem::path&)`, and a manifest-backed `Processor final : public juce::AudioProcessor`. The first writer accepts an empty executable payload for controlled feasibility exports. Its manifest is stored in `RT_RCDATA` resource 101.

- [x] Add failing `Factory.DistinctBindings`, `Factory.ControllerIdentity`, `Factory.HeadlessParameters`, and `Factory.RenameStable` cases. Assert two manifests produce different class IDs, controller references match registered IDs, parameters exist before editor creation, and a renamed copy retains identity.
- [x] Implement the initial PE resource writer using a temporary sibling output and atomic publication. Build effect and instrument template targets with `VST3_AUTO_MANIFEST FALSE`, `COPY_PLUGIN_AFTER_BUILD FALSE`, and `JUCE_VST3_CAN_REPLACE_VST2=0`. Export the inner x64 VST3 binary as the single-file template.
- [x] Adapt the pinned JUCE source through a checked patch during dependency preparation. Verify the expected source revision and support exactly one application of the patch. Route factory metadata, processor/controller IDs, component/controller relationships, compatibility IDs, and parameter numeric IDs through the module binding.
- [x] Implement the minimal processor with manifest-backed parameters and bus layout. Its feasibility-only audio behavior is controlled passthrough or silence, selected by the test fixture.
- [x] Build `NyteraFactoryProbe`, export two synthetic bindings from the same template, and run the four CTest cases. The probe must load both modules in one process and instantiate processor/controller pairs independently.
- [x] Locate REAPER or arrange a dedicated portable installation. Scan the two single files through its dedicated test profile. Verify separate FX entries, names/vendor, parameter lists, and correct identity after save/reopen and rename. Record host version and evidence in `docs/testing/reaper-acceptance.md`.
- [x] Treat this as feasibility gate A. A failure requires diagnosis or a revised design before advancing the product path. Commit only after the automated identity checks and actual REAPER scan succeed.

## Task 3. Load and inspect VST2 sources in native helpers

**Files**

Create `src/helper/Main.cpp`, `Vst2Instance.*`, `tests/fixtures/FixtureVst2.cpp`, and `tests/Vst2Tests.cpp`. Modify CMake to build helper and fixture targets in both architectures.

**Interfaces**

Produces `Result<std::unique_ptr<Vst2Instance>> Vst2Instance::open(const std::filesystem::path&)`, `Result<Manifest> Vst2Instance::inspect()`, `Status Vst2Instance::prepare(const ProcessConfig&)`, and `void Vst2Instance::close() noexcept`. The helper exposes `--inspect <dll> --output <manifest>` for diagnostics.

- [x] Add failing `Vst2.EntryCallback`, `Vst2.InvalidDll`, `Vst2.WrongArchitecture`, `Vst2.NoEditor`, `Vst2.Shell`, and `Vst2.CloseOnce` checks. The controlled fixture issues a host callback before entry-point return and records lifecycle calls.
- [x] Implement architecture-aware loading, `VSTPluginMain` and supported legacy entry fallback, ABI validation, early callback routing, open/close, and metadata inspection using the pinned Xaymar headers. Validate claimed processing functions and fixed topology.
- [x] Extend the fixture with flags for chunk/parameter state, instrument/effect mode, latency, MIDI echo, resizing, malformed metadata, crash, and stall. Keep those controls in the fixture's exported test API or test manifest.
- [x] Build helpers and fixtures with `cmake --build --preset x64-debug` and `cmake --build --preset x86-debug`. Run `ctest --test-dir out/build/x64 -C Debug -R Vst2 --output-on-failure` using both staged architectures.
- [x] Inspect the four supplied DLLs through matched helpers. Record actual metadata and any loading or activation diagnostic. Preserve the original DLL files during all tests.
- [x] Commit the native loading and inspection deliverable.

## Task 4. Establish instance-owned process and control communication

**Files**

Create `src/common/Wire.*`, `src/bridge/Control.*`, `BridgeClient.*`, `tools/BridgeProbe.cpp`, and `tests/ControlTests.cpp`. Modify helper dispatch.

**Interfaces**

Produces `Status BridgeClient::start(const Manifest&, const std::filesystem::path& helper, const std::filesystem::path& source)`, `Result<Bytes> BridgeClient::request(ControlCommand, std::span<const std::byte>, std::chrono::milliseconds)`, `void BridgeClient::stop() noexcept`, and asynchronous notification delivery to the processor's non-audio dispatcher.

- [x] Add failing `Control.CrossBitness`, `Control.VersionMismatch`, `Control.PartialFrame`, `Control.StartupCallback`, and `Control.ParallelInstances` tests. Assert x64-to-x86 communication, explicit version rejection, bounded length decoding, callback ordering, and independent instance IDs.
- [x] Implement a version-1 little-endian frame header with magic, protocol version, opcode, request ID, and payload length. Set explicit opcode-specific size limits. Serialize control requests on a non-audio thread.
- [x] Create per-instance named pipes with restricted access, unpredictable endpoint names, and a launch token. Start helpers hidden with explicit executable paths and quoted arguments. Inherit only explicitly required handles, and use a job object to terminate instance-owned children after owner teardown.
- [x] Use a 60-second startup deadline, 10-second state deadline, and 2-second graceful shutdown deadline. Notifications complete outside a synchronous request cycle. Startup callbacks obtain locally cached supported answers.
- [x] Run `ctest --test-dir out/build/x64 -C Debug -R Control --output-on-failure`. Verify no orphan helper remains after each successful or failed case.
- [x] Commit the cross-architecture control deliverable.

## Task 5. Prove original-editor embedding across the bridge

**Files**

Create `src/helper/EditorHost.*`, `src/plugin/Editor.*`, and `tests/EditorTests.cpp`. Modify bridge control and processor editor creation.

**Interfaces**

Produces `Status BridgeClient::openEditor(uint64_t parentWindow, float scale)`, `void BridgeClient::closeEditor() noexcept`, and helper-side `Status EditorHost::open(HWND parent)`, `void EditorHost::close() noexcept`. Resize notifications carry `Size`.

- [x] Add failing `Editor.Reopen`, `Editor.Resize`, and `Editor.OwnerTeardown` fixture cases. Assert a source editor receives its expected parent, resize reaches the JUCE container, and close is idempotent after partial startup.
- [x] Implement the native helper UI thread, editor-open/close/idle lifecycle, HWND serialization with range checks, focus routing, native container lifetime, and resize notifications. Apply a deliberate DPI context before creating helper windows.
- [x] Run `ctest --test-dir out/build/x64 -C Debug -R Editor --output-on-failure` for x86 and x64 fixtures. Then test original VPRE-73 and Decapitator editors through the diagnostic host.
- [x] Check mouse capture, typing, menus, dialogs, open/close cycles, and display scaling in the diagnostic host. Record observed plugin-specific behavior. The REAPER presentation check follows in Task 6 once full payload extraction is available.
- [x] Commit the diagnostic-host editor deliverable when controlled fixtures and available source editors pass, recording any concrete plugin prerequisite. Feasibility gate B completes in Task 6 before audio and generator work advances.

## Task 6. Package all runtime assets and extract safely

**Files**

Extend `src/package/Package.*`. Create `src/package/Cache.*` and `tests/PackageTests.cpp`. Modify module startup to use extracted payloads.

**Interfaces**

Extends `writePackage` to the complete `PackageInput`. Produces `Result<ExtractedPayload> ensurePayload(HMODULE module, const Manifest&, const std::filesystem::path& cacheRoot)`, where `ExtractedPayload` owns a lease and exposes source/helper paths.

- [x] Add failing `Package.Relocate`, `Package.ConcurrentExtract`, `Package.CorruptCache`, `Package.UnicodePaths`, `Package.PathEscape`, and `Package.AtomicExport` tests. Assert reconstruction after cache removal, two simultaneous leases, hash verification, Unicode paths, rejection of traversal/reparse escapes, and preservation of an existing output on failed export.
- [x] Embed manifest as resource 101, helper as 102, source DLL as 103, and support files as 1000 upward. Reserve metadata describing each hash, length, and destination. Include only the matched helper in each output.
- [x] Implement staging and atomic publication at export and extraction. Validate the resolved destination root, reject absolute/reserved/escaping support paths and reparse destinations, and preserve safe relative layouts. Load dependencies using controlled source/support search directories.
- [x] Use `%LOCALAPPDATA%/Nytera/VST2Wrapper/Cache/<payload-hash>-<runtime-version>` with cross-process publication coordination and an open-file lease. Cleanup removes completed stale entries only after a 30-day idle period and successful exclusive lease acquisition.
- [x] Run `ctest --test-dir out/build/x64 -C Debug -R Package --output-on-failure`. Exercise a relocated self-contained fixture package through REAPER after excluding its original source directory from the test environment.
- [x] Complete feasibility gate B by opening the x86 Decapitator and x64 VPRE-73 conversions in REAPER's FX chain and floating window. Verify the Task 5 interactions and record any source-plugin prerequisite. Resolve embedding failures in the controlled fixtures before advancing to Task 7.
- [x] Commit the single-file packaging and cache deliverable.

## Task 7. Translate audio, MIDI, and transport with measurable latency

**Files**

Create `src/bridge/AudioTransport.*` and `tests/AudioTests.cpp`. Extend helper processing and `src/plugin/Processor.*`.

**Interfaces**

Produces `Status BridgeClient::prepare(const ProcessConfig&)`, `ProcessResult BridgeClient::process(AudioBlockView, EventBlockView, const TransportSnapshot&) noexcept`, `uint32_t BridgeClient::latencySamples() const noexcept`, and `void BridgeClient::resetProcessing() noexcept`. Helper-side `Status Vst2Instance::process(AudioBlockView, EventBlockView, const TransportSnapshot&)` performs the source call. `AudioBlockView` holds spans of channel spans and a sample count. `EventBlockView` holds bounded input/output event storage. `ProcessResult` distinguishes rendered, priming, underrun, and failed states.

- [x] Add failing `Audio.ImpulseLatency`, `Audio.VariableBlocks`, `Audio.ZeroBlock`, `Audio.SeekFlush`, `Audio.MidiAlignment`, `Audio.DoubleInput`, `Audio.Offline`, and `Audio.CallbackAllocations` tests. Assert exact delayed impulse position, ordered events, transport-aligned output, safe conversion of precision, deterministic offline results, and allocation-free prepared callbacks.
- [x] Choose quantum `Q = nextPowerOfTwo(maximumExpectedSamplesPerBlock)` at preparation. Use a two-quantum pipeline delay, bounded shared-memory queues, and an adapter for blocks up to the prepared maximum. Zero-length blocks preserve sample position. Reconfiguration allocates outside processing and flushes the prior generation.
- [x] Define shared-memory offsets explicitly and use documented cross-process synchronization primitives with memory-order guarantees. Transmit audio, timestamps, MIDI/SysEx, parameter events, and transport snapshots without native pointer serialization. Validate sequence/generation and all byte ranges.
- [x] Process on a helper-owned worker thread. In real time, consume only completed output and report underruns for supervision. In offline mode, use ordered completion with a 30-second finite processing deadline. Report `2 * Q + sourceLatency` to REAPER and verify the measured value.
- [x] Forward bypass, tail, precision conversion, supported MIDI capabilities, timing validity, and fixed multichannel routing. Discontinuities discard stale events and release held notes through the instance's event path.
- [x] Run `ctest --test-dir out/build/x64 -C Debug -R Audio --output-on-failure` with block sequences including 0, 1, 63, 64, 127, 256, and 511 samples within the configured maximum. Verify normal and offline rendering through REAPER with an impulse and the controlled MIDI instrument.
- [x] Commit the audio/event translation deliverable and measured latency results.

## Task 8. Relay automation and host information bidirectionally

**Files**

Complete `src/plugin/Parameter.*`. Modify common event codecs, helper callbacks, and processor notifications. Create `tests/ParameterTests.cpp`.

**Interfaces**

Produces `void BridgeClient::enqueueParameter(const ParameterEvent&) noexcept`, parameter-value/gesture notifications, and manifest-backed `Parameter final : public juce::AudioProcessorParameterWithID` with stable numeric IDs provided by the JUCE adaptation.

- [x] Add failing `Parameter.StableIds`, `Parameter.Gestures`, `Parameter.NoEcho`, `Parameter.Display`, and `Parameter.TopologyChange` tests. Assert IDs across regenerate/reload, one gesture pair per edit, bounded event propagation without feedback, correct source text or normalized fallback, and a recoverable failure on changed topology.
- [x] Implement host-to-source changes and source-to-host automation notifications with origin markers and ordered event delivery. Expose names, labels, automatable flags, initial values, source-supported text formatting, and normalized fallback. Keep formatted-display requests away from audio-thread blocking.
- [x] Translate latency/display/program/state-dirty/editor-size callbacks. Cache supported host identity and capability answers inside the helper. Use JUCE host notifications on their appropriate threads.
- [x] Verify the pinned JUCE backend's automation timing behavior and document its supported resolution. Avoid claiming precision exceeding the measured interface behavior.
- [x] Run `ctest --test-dir out/build/x64 -C Debug -R Parameter --output-on-failure`. Record automation from each supplied editor that loads successfully, then replay it with the editor closed in REAPER.
- [x] Commit parameter and host-callback translation.

## Task 9. Persist complete instance state and restore before playback

**Files**

Create `src/common/State.*` and `tests/StateTests.cpp`. Extend helper state commands, bridge client, and processor state overrides.

**Interfaces**

Produces `Bytes encodeState(const StateEnvelope&)`, `Result<StateEnvelope> decodeState(std::span<const std::byte>)`, `Result<StateEnvelope> BridgeClient::captureState()`, and `Status BridgeClient::restoreState(const StateEnvelope&)`. Helper-side `Result<StateEnvelope> Vst2Instance::captureState()` and `Status Vst2Instance::restoreState(const StateEnvelope&)` own the source API calls.

- [x] Add failing `State.ChunkExact`, `State.ParameterFallback`, `State.BeforeStartup`, `State.CaptureTimeout`, `State.IdentityMismatch`, and `State.InstanceIsolation` tests. Assert exact chunk bytes, program/parameter restore, queued early state, retention of the last valid complete snapshot, explicit binding mismatch, and independent instances.
- [x] Implement a bounded version-1 state codec. Copy plugin-owned chunks immediately. Provide a current-program/parameter fallback for sources lacking chunks and refresh values after restore.
- [x] Serialize source processing and state operations on the helper side. Finish a snapshot only after receiving a complete framed response. Apply queued restore before rendering audible output and invalidate queued audio/events from the prior state.
- [x] Connect `getStateInformation` and `setStateInformation` to the complete snapshot and queued-restore paths. Preserve current valid state through startup, editor closure, reconfiguration, and transient helper failure.
- [x] Run `ctest --test-dir out/build/x64 -C Debug -R State --output-on-failure`. Save/reopen a REAPER project with two different states of each available supplied plugin, including closed editors, duplication, and undo/redo.
- [x] Commit the state and project-recall deliverable.

## Task 10. Bound failures and recover the same binding

**Files**

Extend `src/bridge/BridgeClient.*`, helper shutdown, processor failure handling, and editor diagnostics. Create `tests/RecoveryTests.cpp`.

**Interfaces**

Produces `BridgeStatus BridgeClient::status() const noexcept`, `Status BridgeClient::retry()`, and structured runtime failure notifications. `BridgeStatus` includes starting, ready, failed, and stopped states plus the last diagnostic.

- [x] Add failing `Recovery.CrashDuringState`, `Recovery.HungHelper`, `Recovery.RetryBinding`, `Recovery.TeardownRace`, and `Recovery.MalformedMessage` tests. Assert saved snapshot survival, finite deadlines, unchanged conversion identity on retry, instance-owned cleanup, and safe protocol rejection.
- [x] Supervise process exit and control disconnects outside the audio callback. A failed instance outputs silence, clears held-note tracking, retains recoverable state, and exposes a concise diagnostic when its editor opens.
- [x] Retry by extracting/verifying the same payload, starting the same helper architecture, preparing it, restoring the last complete snapshot, and reopening the original editor if requested. Serialize retry against teardown.
- [x] Enforce all earlier deadlines and job ownership. Test partial startup, closing the DAW while an editor request is pending, and simultaneous healthy/failed instances.
- [x] Run `ctest --test-dir out/build/x64 -C Debug -R Recovery --output-on-failure`. Confirm the crashing/stalling controlled fixture cannot leave an orphan helper after test teardown.
- [x] Commit failure handling and recovery.

## Task 11. Deliver the standalone Nytera generator

**Files**

Create `src/generator/Main.cpp`, `MainComponent.*`, `Conversion.*`, `tests/ConversionTests.cpp`, and necessary release staging rules. Reuse `Package` and `BridgeClient` directly.

**Interfaces**

Produces `Result<InspectionResult> inspectSource(const std::filesystem::path&, const RuntimeAssets&)` and `Status generateConversion(const ConversionRequest&, const RuntimeAssets&, ProgressCallback)`. `RuntimeAssets` locates shipped templates and helpers. `ConversionRequest` carries source, optional support directory, and output path.

- [x] Add failing `Conversion.MatchedHelper`, `Conversion.MetadataMatchesOutput`, `Conversion.NoCompiler`, and `Conversion.CancelPreservesOutput` tests. Assert matched architecture, factory-visible exported metadata, successful export with developer tools absent from PATH, and transactional cancellation.
- [x] Implement asynchronous DLL/support-directory selection, inspection, metadata summary, test launch, destination selection, progress, and actionable errors. The visible application title is `Nytera VST2 Wrapper`. Default output filename is the sanitized source name plus `.vst3`.
- [x] Select effect/instrument templates and expose MIDI capabilities consistently. Prefer two category templates with runtime capability reporting when the patched backend permits it. Add only compile-time variants proven necessary by the capability tests.
- [x] Include PE import/dependency diagnostics and Windows loader failure details. Allow the user to supply support files and retest. Preserve plugin-specific installation prerequisites in the inspection report.
- [x] Run `ctest --test-dir out/build/x64 -C Debug -R Conversion --output-on-failure`. Generate wrappers for the four supplied DLLs using the GUI and compare them with the diagnostic export path.
- [x] Commit the usable standalone conversion application.

## Task 12. Verify the complete release path in REAPER

**Files**

Create `scripts/Build.ps1`, `scripts/Verify.ps1`, and `README.md`. Complete `docs/testing/reaper-acceptance.md` and release staging rules.

**Interfaces**

`Build.ps1 -Configuration Release` stages `out/package/NyteraVST2Wrapper.exe`, architecture-matched helper assets, effect/instrument templates, and required notices. `Verify.ps1 -ReaperPath <absolute-path> -Configuration Release` runs automated suites and opens the dedicated REAPER acceptance profile as needed. Manual checks remain explicitly recorded.

- [x] Build both release architectures and stage only generator/runtime assets and dependency notices. Confirm the supplied commercial DLLs stay outside the staged application.
- [x] Run `cmake --build --preset x64-release`, `cmake --build --preset x86-release`, and the release equivalents of every registered CTest case. Expected result is all applicable automated cases passing.
- [x] Run generation from the staged application with development tools absent from PATH. Install generated single files only into the dedicated REAPER profile's search directory and scan them there.
- [x] Complete the design's twelve acceptance checks with the four supplied plugins and controlled fixtures. Record REAPER/OS/plugin versions, editor evidence, latency measurements, real-time/offline behavior, and exact load/activation failures where applicable.
- [x] Exercise relocation, cache reconstruction, multiple instances, project save/reopen, automation recording/replay, editor closure, DPI changes, MIDI instrument playback, and helper recovery through normal application entry points.
- [x] Write concise build, generation, installation, cache, and diagnostic instructions. Distinguish observed plugin compatibility from remaining prerequisites in the acceptance matrix.
- [x] Self-review the complete diff, run the requested execution method's final independent review, resolve findings, and rerun affected verification before committing the release-ready deliverable.

## Completion conditions

Deliver the built standalone generator, its runtime assets, the source project, and the acceptance results. Report the exact build/test commands and their outcomes. A successful source build alone leaves REAPER/editor acceptance outstanding. A plugin-specific external prerequisite is reported against that plugin while the controlled fixtures establish bridge behavior.

If either feasibility gate fails, carry diagnosis through the permitted design. A necessary change to single-file output, embedded payloads, original-editor presentation, or end-user compiler requirements returns to the user with concrete evidence and a proposed design revision.

## Planning references

- [JUCE 8.0.13 CMake API](https://github.com/juce-framework/JUCE/blob/8.0.13/docs/CMake%20API.md) documents target configuration, automatic VST3 manifests, and post-build installation behavior.
- [Pinned JUCE VST3 client](https://github.com/juce-framework/JUCE/blob/8.0.13/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp) is the adaptation baseline.
- [Pinned Xaymar SDK](https://github.com/Xaymar/vst2sdk/tree/339d4f31590bf77c0d0d248e09a380ac6285e069) supplies source-plugin interface definitions.

## Plan self-review

Coverage maps generation/factory identity to Tasks 1, 2, 6, and 11, helpers and lifecycle to Tasks 3, 4, and 10, original editors to Task 5, audio/MIDI/transport to Task 7, automation/information to Task 8, state to Task 9, and release acceptance to Task 12. The five review-focus conditions have named tests in their owning tasks. Shared interfaces use the names and types in this document. Commercial-plugin execution and actual REAPER evidence remain explicit acceptance work.
