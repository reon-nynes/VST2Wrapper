# Nytera VST2 Wrapper

A Windows application that generates a dedicated **single-file x64 VST3** from an x86 or x64 VST2 DLL. Each output embeds its source DLL and matching helper. The converted plugin presents the source's original editor in REAPER.

## Run the application

Build output is staged in `out/package`. Keep `NyteraVST2Wrapper.exe`, the `runtime` directory and `notices` together.

1. Open `NyteraVST2Wrapper.exe`.
2. Choose or drop a VST2 DLL. Select **Inspect** to read its architecture, parameters, routing and dependencies.
3. If the source needs portable companion files, select their root under **Support files** and inspect again. The directory structure is preserved relative to the embedded DLL.
4. Use **Test original editor** to open the source interface from the same packaged/extracted runtime used by a conversion.
5. Choose an output ending in `.vst3`, then select **Generate VST3**.
6. Copy the resulting file to your REAPER VST search folder and rescan under **Preferences → Plug-ins → VST**.

Export runs with the shipped templates and helpers. A compiler is required to build this project. End-user generation uses the staged application assets.

Each output has a permanent binding. Converting different source bytes creates a different plugin identity. Regenerating with the same DLL preserves source-derived class and parameter IDs. Renaming the generated file preserves its identity.

The source's installation, activation, external data and installed runtimes remain prerequisites when the source relies on them. The import report lists statically declared DLL imports. Additional files loaded dynamically can be identified by the source's own diagnostics. Select portable support files where applicable and retest.

## State, cache and recovery

REAPER projects retain a checksummed per-instance state envelope. The bridge copies VST2 bank/preset chunks when supported and falls back to the current program and reported parameters. It applies early project state before audible processing. The embedded source DLL remains in the `.vst3` file.

For a source without a bank chunk, the fallback captures its selected program and reported values. Edits held only in other programs depend on the source's bank-state support.

The runtime extracts verified payloads automatically under `%LOCALAPPDATA%\Nytera\VST2Wrapper\Cache`. Instances share complete content-addressed payloads and keep an active lease. Idle cache entries are eligible for cleanup after 30 days. A missing cache is reconstructed from the converted file.

A failed instance outputs silence and preserves its last complete captured state. Its editor displays a diagnostic and **Retry this plugin**. Retry verifies the same payload, relaunches its matched helper and restores the recoverable state. Each instance owns its helper job. Closing a project or the application cleans up those processes with bounded waits.

## Supported bridge behavior

- x86 and x64 Windows VST2 sources inside x64 VST3 outputs.
- Effects and MIDI instruments with an original editor and fixed parameter/channel topology.
- Audio, normalized parameters, gestures, source display text, transport information, MIDI and SysEx.
- Real-time pipelining and finite ordered offline/prefetch rendering, including REAPER background-thread detection.
- Reported latency of `2 * nextPowerOfTwo(maximumHostBlock) + sourceLatency` samples.
- Host float64 buffers converted through the float32 transport.
- Automation at the pinned JUCE adapter's measured host-block resolution.
- Up to 4096 MIDI events and 1 MiB of MIDI bytes per quantum, with preallocated large-SysEx storage.
- Explicit diagnostics for shells, changing topology, invalid metadata, missing dependencies and helper failures.

Source plugins differ in their dependence on host-specific behavior and external components. Observed compatibility and outstanding prerequisites are recorded in [REAPER acceptance results](docs/testing/reaper-acceptance.md).

## Build

Use Windows x64 with CMake 3.25 or newer, Git, and Visual Studio 2022 Build Tools with the v143 C++ x86/x64 toolchain and a Windows SDK. Dependencies are pinned and fetched into `out/deps`.

```powershell
pwsh -File scripts/Build.ps1 -Configuration Release
pwsh -File scripts/Verify.ps1 -Configuration Release
```

The mandatory automated suites use controlled fixtures built from this repository. Commercial DLLs are optional inputs for the separate compatibility checks.

Equivalent architecture commands use the `x86-release` and `x64-release` configure/build presets. Build both architectures before the `NyteraStage` target. Debug builds use `-Configuration Debug` and the corresponding debug presets.

To run controlled host acceptance with an isolated profile, supply an absolute REAPER executable path.

```powershell
pwsh -File scripts/Verify.ps1 -Configuration Release `
  -ReaperPath 'C:\Program Files\REAPER (x64)\reaper.exe' -AcceptanceScript Audio
```

The verification script creates its profile and generated fixture plugins under `out/reaper-acceptance`, then starts a separate REAPER instance. It supports the `Audio`, `State`, `Automation`, `Editors`, `Recovery` and `Relocation` scripts. Close completed-render dialogs to advance between projects. The automation case requests one click inside the fixture's original editor. Recovery exercises the first helper's failure and the Retry button. Relocation removes a temporary source copy and renames the generated fixture before host loading. The editor case expects Decapitator, LittleAlterBoy, BBE Sonic Maximizer and VPRE-73 exports in `out/gui-generated`.

## Dependencies and source layout

The project uses JUCE 8.0.13 and the pinned Xaymar VST2 SDK. The checked JUCE patch provides runtime VST3 identity, source-derived parameter IDs, MIDI capabilities, large-SysEx storage, per-block processing policy and combined-controller state synchronization. It is validated and applied during configuration.

Dependency licenses are copied to `out/package/notices`. Consult those files for the applicable JUCE, Xaymar SDK and VST3 SDK terms. The supplied commercial demo DLLs remain local fixtures in ignored `vst2demo` and stay outside the staged application.

Core code is organized under `src/common`, `src/bridge`, `src/helper`, `src/package`, `src/plugin` and `src/generator`. The approved design and implementation plan are under `docs/superpowers`.

## Releases

The Windows release workflow runs when a `v*` tag is pushed or a GitHub release is published. It builds the x64 generator and templates plus both helper architectures on Windows Server 2022, runs the native test suites, and attaches the Windows ZIP, matching source ZIP and SHA-256 checksums to the tagged release. A manual workflow run on a branch uploads build artifacts for verification.

```powershell
git tag v0.1.0
git push origin v0.1.0
```

The Windows ZIP contains the staged application and runtime assets. The source ZIP includes the tagged project, patched JUCE sources, the pinned VST2 SDK and dependency Git metadata for an offline dependency rebuild. Build it with the same `scripts/Build.ps1` command. Packaging can also run locally after committing the source and building Release.

```powershell
pwsh -File scripts/Package.ps1 -Version v0.1.0
```

## License

Copyright (C) 2026 Nytera contributors. Nytera's original code is licensed under the GNU General Public License version 3 only, available in [LICENSE](LICENSE).

JUCE 8.0.13 and the modifications in the JUCE patch use JUCE's AGPLv3 open-source terms. The full text is included in [LICENSES/AGPL-3.0-only.txt](LICENSES/AGPL-3.0-only.txt). GPLv3 section 13 permits combining GPLv3 and AGPLv3 code. Distribution of the combined application must satisfy the applicable AGPLv3 requirements as well. Third-party code retains its upstream license terms and release packages include the dependency notices. [JUCE license](https://github.com/juce-framework/JUCE/blob/8.0.13/LICENSE.md)
