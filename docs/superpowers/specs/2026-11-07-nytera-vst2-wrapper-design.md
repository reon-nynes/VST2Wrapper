# Nytera VST2 wrapper generator

Date 2026-11-07

Status. Approved by the user on 2026-11-07. Native implementation is in progress.

## Intent and agreed requirements

The user wants to use old Windows VST2 plugins in a modern DAW. REAPER x64 is the first target. Nytera is the company name. Development uses C++, JUCE, CMake, and the Xaymar VST2 SDK project.

The chosen approach is a standalone generator application. Selecting a VST2 DLL creates a dedicated 64-bit VST3 permanently bound to that plugin. Both x86 and x64 VST2 DLLs are supported. Each output is a single physical `.vst3` file with its VST2 DLL and bridge runtime embedded. Runtime extraction into a per-user cache is part of the proposed design.

The generated plugin presents the original VST2 editor inside REAPER's plugin window. Audio, MIDI, parameters, automation, transport information, and saved state cross the translation layer. Each project instance restores its own state and the binding to its packaged DLL.

The user selected the dedicated generator approach, requested single-file packaging and the original editor, and approved this document. Engineering choices originally proposed by the assistant are identified below for provenance.

## Approaches considered

### Dedicated plugin generation, selected

One generated VST3 has one fixed VST2 binding and a known initial parameter layout. REAPER can discover the plugin identity and parameters before opening an editor. The standalone generator handles DLL selection. This fits automation identity and reliable project recall.

### Universal wrapper with first-use selection

One installed VST3 could choose a DLL per project instance. With the standard JUCE VST3 wrapper, a fixed bank of automation slots would provide stable enumeration. This adds a parameter limit and complicates metadata refresh. The user chose dedicated generation after discussing this trade-off.

### Packaging choice

A VST3 directory bundle provides the current documented Windows layout. The user requested one physical file, so the design uses the legacy single-file Windows VST3 layout and embeds payloads as executable resources. Steinberg marks this layout deprecated beginning with SDK 3.6.10. REAPER acceptance is an early feasibility condition and requires an actual scan and load test.

## Product boundaries

Initial delivery targets Windows x64 and REAPER x64. The source plugins may be Windows x86 or x64 VST2 audio effects or instruments. Windows ARM and other DAWs are later compatibility work.

Each generated file has an immutable source-plugin binding. Creating another conversion produces another output. A binding identity stays stable when regenerating the same conversion with a compatible wrapper runtime.

The original editor is the successful-load interface. Conversion requires a source plugin with an editor. Editor-less plugins receive a clear conversion diagnostic. REAPER supplies its ordinary host window and automation controls.

Standalone VST2 plugins are the initial scope. Shell DLLs that expose multiple subplugins receive an explicit unsupported diagnostic in this version. Support for arbitrary shell enumeration is separate work.

Embedding covers the chosen DLL, the appropriate bridge executable, and explicitly supplied support files. The generator can package a user-selected support directory while preserving relative paths. Existing activation services, registry-based installation requirements, and external sample libraries remain compatibility prerequisites when a plugin requires them. The generator reports detected dependencies and allows a test launch before export.

## Architecture

### Generator application

Build the standalone Windows GUI with JUCE and CMake. Its flow is DLL selection, optional support-directory selection, inspection, output selection, and generation. Inspection runs in an architecture-matched helper. The helper returns validated metadata and a fingerprint of the inspected payload. The application provides a concise success report or an actionable failure message.

The generator exports from prebuilt wrapper templates and embeds resources using Windows APIs. This toolchain-free generation is an assistant proposal. A user running the released generator needs its shipped templates and helpers. Compiler tools are development dependencies.

Metadata includes source name and vendor, source unique ID, architecture, payload hash, effect/instrument category, channel layout, MIDI capabilities, parameter descriptors, original editor dimensions, latency, tail information, wrapper schema version, and conversion identity. Unknown plugin capabilities are represented explicitly and checked during inspection.

### Generated x64 VST3

The file contains the x64 JUCE-based VST3 binary, a binding manifest, the source DLL and support files, and the required helper executable. Each output has a unique processor class ID and controller class ID, a source-derived display name, and Nytera as wrapper vendor. Original vendor information is retained in the manifest.

The factory reads metadata from its own module resources. It can enumerate identity and category during scanning. The processor constructs its complete parameter proxies and bus description from the manifest before JUCE creates its VST3 parameter mapping. Plugin operation and state restore work independently of editor creation.

Stock JUCE uses compile-time plugin identity in several VST3 paths. A maintained, narrowly scoped VST3 client adaptation must route factory registration, controller relationships, instance class checks, reported IDs, category, and display metadata through the embedded manifest. Factory class enumeration, `getControllerClassId`, and saved-project identity must agree. Renaming a template file alone cannot provide this behavior.

The implementation plan must begin with proving distinct generated identities and single-file scanning in REAPER. If JUCE adaptation cannot satisfy these requirements, the design returns for revision before adopting a compiler requirement or changing the requested output.

Template variants may cover compile-time effect/instrument and MIDI capability combinations. Every variant uses the same manifest schema and bridge protocol. The planner chooses the smallest variant set that accurately reports capabilities.

### Bridge helpers

Ship architecture-matched x86 and x64 helper builds. Each generated instance starts one helper process dedicated to that instance. Using helpers for x64 sources as well is an assistant proposal that gives both architectures the same lifecycle and failure handling.

The helper loads the source through the normal Windows loader and implements its VST2 host interface using Xaymar's definitions. Use native Win32 facilities for the helper wherever this avoids imposing JUCE's platform support requirements on x86 builds. The helper owns the DLL, VST2 instance, editor thread, processing thread, and plugin-state operations.

Plugin startup, shutdown, editor operations, and processing observe the VST2 lifecycle and correct calling conventions. Validate entry points, ABI magic, architecture, pointer fields, and resource limits before trusting metadata. Account for callbacks issued during entry-point invocation and initialization.

### Cache and payload lifecycle

Extract to `%LOCALAPPDATA%\Nytera\VST2Wrapper\Cache` under a content-derived payload/version directory. Keep payload extraction separate from per-instance mutable data. Preserve DLL filenames and support-directory layout for relative asset lookup.

Use atomic publication and coordination between simultaneous instances. Validate existing files against embedded hashes. A partially extracted directory is never treated as complete. Open payloads are protected from cleanup until their helpers exit. Failures leave an actionable diagnostic and can be retried after the underlying issue is resolved.

The source folder used during generation can be removed for self-contained plugins once the output has passed relocation testing. Plugins with external installation requirements must also be tested with their required environment.

## Audio and event translation

Use shared memory for audio blocks and timestamped event queues. A separate control connection carries initialization, editor requests, metadata, diagnostics, and state blobs. Define a versioned wire format with fixed-width values, explicit lengths, bounds, sequence numbers, and offsets. Cross-process messages carry handles or protocol identifiers appropriate for their operation. Process-local C++ pointers stay inside their owning process.

Preallocate the real-time transport at preparation time. File I/O, extraction, process startup, state serialization, and unbounded waits execute outside the audio callback. The bridge uses a fixed-quantum audio pipeline with bounded queues. Report its actual algorithmic delay plus source latency to REAPER. The planner selects the initial quantum and queue design, documents the latency, and verifies variable-size host blocks through an adapter.

Each audio quantum carries the corresponding transport snapshot and MIDI/event offsets. Preserve event ordering and audio/event alignment through the pipeline. Forward playback state, tempo, time signature, sample position, loop information, sample rate, and processing mode where available. Mark unavailable timing fields accordingly.

Support normal single-precision processing. Adapt a double-precision host path to the plugin's supported precision. A plugin's advertised processing function must exist and be usable. REAPER offline rendering uses deterministic ordered processing with a separate bounded worker deadline suitable for offline work. Flush pending audio and events correctly on transport discontinuities and reconfiguration.

Expose the inspected main and additional audio channels using stable bus/pin descriptions. The first version supports fixed topology discovered during conversion. A plugin that changes parameter count or channel topology receives a compatibility failure that preserves the last recoverable state.

## Parameters and host information

Assign a stable VST3 parameter ID from conversion identity and original VST2 parameter index. Resolve collisions deterministically during generation and persist the mapping. Renaming or moving the generated file preserves IDs. Duplicate copies of the same conversion represent the same plugin identity.

Expose original names, labels, normalized values, display formatting, defaults where reliably available, and automatable status. The normalized VST2 range is the underlying value representation. Use plugin-supported conversion callbacks for display text when available, with a documented normalized fallback.

Relay host automation to the plugin and plugin editor automation back to REAPER, including begin/end edit gestures. Prevent feedback loops and preserve ordering. JUCE's parameter/event model and VST2 capabilities determine automation resolution. Validate recorded and replayed automation at the supported resolution.

Source host callbacks update latency, editor size, parameter values, display information, and state-dirty notifications through the appropriate queues. Reentrant calls are handled without a synchronous cross-process callback cycle. Source host identity and capability queries receive consistent supported answers. Host-specific extensions require deliberate translation.

## Original editor integration

The helper opens the VST2 editor against a native child-window container supplied by the generated VST3 editor. The JUCE editor provides the host attachment and lifetime boundary. The visible plugin content is the original editor.

Bridge editor-open, close, idle, size queries, and resize requests. Verify mouse capture, keyboard focus, typing into controls, menus, file dialogs, repeated open/close, and both REAPER FX-chain and floating-window presentation. Handle per-monitor DPI behavior deliberately. Microsoft documents DPI complications for cross-process parenting, so embedding is a feasibility condition requiring a real legacy editor test.

The standalone generator owns inspection and conversion diagnostics. A failed runtime load or disconnected helper may present a concise Nytera error panel in place of the unavailable editor. This exceptional panel is an assistant proposal. It identifies the failure and offers retry of the same binding.

## State and identity persistence

Save a versioned wrapper-state envelope containing conversion identity, payload hash, source identity, state encoding, current program information, and source state. Use opaque VST2 chunks when supported. Otherwise save program/parameter data sufficient to restore the plugin's reported state. Preserve opaque chunks byte-for-byte and validate serialized sizes.

The DLL is already embedded in the generated file. REAPER project state stores the per-instance state envelope and identifies the required conversion. Capture and restore work with the editor closed. Coordinate state operations and processing on the helper side for consistent snapshots. A timeout or failed snapshot keeps the last valid snapshot available and reports failure without accepting a partial blob.

When a state arrives before the helper is ready, retain it and apply it before processing becomes audible. Restore the source state, refresh parameter caches, and publish resulting latency and display information in a consistent order. Multiple instances remain independent across saves, reloads, duplication, and undo/redo.

A conflicting conversion identity or payload hash produces an explicit compatibility diagnostic. Regeneration with compatible runtime code preserves the binding and class IDs. Replacing the source DLL with different bytes is a new conversion for the initial version.

## Failures and recovery

Helpers have startup and processing health checks. Detect exit, disconnect, timeout, malformed messages, and invalid payloads. On a failed bridge, output silence for that instance, release stuck notes, retain recoverable state, and surface the error when the editor is opened. Retry restarts the same embedded plugin and reapplies the last valid state.

Use bounded shutdown. Each instance owns its helper and IPC resources. Host teardown, editor close, project switching, and failed initialization all converge on cleanup that can safely run after partial startup.

## Verification and acceptance

The implementation is acceptable when these checks pass in REAPER x64.

1. REAPER scans a generated single-file VST3 and discovers its correct identity, category, and parameters.
2. Two different conversions coexist, and each reloads the correct plugin after project save/reopen.
3. An x86 VST2 and an x64 VST2 both produce audio through their matched helpers.
4. The original editor embeds in FX-chain and floating presentation, supports interaction, and survives repeated open/close and DPI changes.
5. Parameter automation records from the original editor and replays from REAPER with correct parameter identity.
6. MIDI instruments receive correctly timed events and render correctly in real-time and offline modes.
7. Chunk-based and parameter-based state restoration work with the editor closed and with multiple instances.
8. Plugin and bridge latency are reported and verified using a measurable test signal.
9. Variable block lengths, sample-rate changes, transport seeks, bypass, and repeated preparation preserve correct operation.
10. Helper failure, missing dependencies, invalid DLLs, startup timeouts, and simultaneous extraction produce bounded, recoverable behavior.
11. A self-contained conversion works after relocation, source-DLL removal, and cache deletion followed by automatic reconstruction.
12. Generation succeeds on a machine with the released Nytera application and its shipped assets, using the toolchain-free export path.

### User-provided compatibility targets

The user supplied four DLLs in `vst2demo/`. PE headers and Windows version resources were inspected on 2026-11-07.

| DLL | Architecture | Reported product | Reported vendor | Reported version |
| --- | --- | --- | --- | --- |
| `Decapitator.dll` | x86, 32-bit | Decapitator | SoundToys | 5.0.1 |
| `LexHall.dll` | x64, 64-bit | LexHallVST | Harman International | 1.3.8 |
| `LittleAlterBoy.dll` | x64, 64-bit | Little AlterBoy | SoundToys | 5.0.1 |
| `VPRE-73.dll` | x64, 64-bit | VPRE-73 | Black Rooster Audio | 2.5 BUILD 264 |

Use Decapitator for the first cross-architecture editor and processing checks. Start the x64 path with VPRE-73, then exercise the other two x64 plugins for additional editor, state, and dependency behavior. This test order is an assistant proposal.

These findings establish architecture and embedded version metadata. Runtime loadability, required support files or activation, editor behavior, and state behavior require execution tests. Retain the supplied DLLs as local fixtures. Arrange controlled test fixtures for instrument MIDI behavior and state encodings that these four effects may leave uncovered. The planner must arrange a REAPER test installation if the local environment lacks one. Source-only checks cannot establish editor or DAW compatibility.

## Calls left to the builder

Choose pinned dependency revisions, exact UI layout for the generator, internal module boundaries, IPC primitives, pipeline quantum, finite deadlines, cache cleanup policy, and the narrowest maintainable JUCE adaptation. Preserve the requirements and feasibility gates above. Document measurable latency and compatibility results.

The workspace was empty and had no Git repository at inspection time. The approved design is stored in the requested design-document location. Creating the implementation project and establishing source-control history belong to implementation.

## Sources

- [Xaymar VST2 SDK](https://github.com/Xaymar/vst2sdk) supplies the header-only interface definitions.
- [JUCE VST3 client source](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp) establishes the need to address initialization-time parameters and template identity paths.
- [Steinberg plugin format structure](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Locations%2BFormat/Plugin%2BFormat.html) documents Windows bundles and the deprecated single-file layout.
- [Microsoft process interoperability](https://learn.microsoft.com/en-us/windows/win32/winprog64/process-interoperability) documents the x86/x64 DLL boundary.
- [Microsoft SetParent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent) documents native parenting and cross-process DPI complications.
- [Microsoft executable resources](https://learn.microsoft.com/en-us/windows/win32/menurc/resources) documents executable payload storage.
- [Microsoft LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw) documents file-based loading and dependency resolution.
- [REAPER VST implementation notes](https://www.reaper.fm/sdk/vst/vst_ext.php) provide source-plugin hosting and concurrency considerations.
