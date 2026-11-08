# REAPER compatibility results

## Environment

Verification ran on Windows 11 Pro build 26200 with portable REAPER 7.85 x64 on November 7–8, 2026. CMake 4.4.5 and Visual Studio 2022 v143 built JUCE 8.0.13 and the pinned Xaymar SDK. The monitor runs at 150% scaling. Physical monitor-to-monitor DPI changes were unavailable on this single-monitor setup.

Host acceptance uses `out/reaper-acceptance`, with its own INI, plugin search directory, cache and process. First-profile migration runs with automatic scanning disabled, then the script sets the explicit test-only VST path. Each launch uses `-newinst`. The audio backend is REAPER Dummy Audio, with a 1024-sample device block and project sample rates of 44.1 or 48 kHz. These playback observations use host meters and rendered sample comparisons.

## Supplied plugin results

| Source | VST2 architecture | Metadata and editor | State and audio | Original-editor automation |
| --- | --- | --- | --- | --- |
| Decapitator 5.0.1 | x86 | 12 parameters, stereo, original 870 × 423 editor | Pass, two differently configured instances, duplication, undo/redo, closed-editor recall, offline and real-time audio | Pass, Drive gesture recorded, envelope saved/reopened and rendered with editor closed |
| Little AlterBoy 5.0.1 | x64 | 7 parameters, stereo, original 870 × 283 editor | Pass, two differently configured instances, duplication, undo/redo, closed-editor recall, offline and real-time audio | Pass, Pitch gesture recorded, envelope saved/reopened and rendered with editor closed |
| BBE D82 Sonic Maximizer | x86 | Nomad Factory, 5 parameters, stereo, original 800 × 160 editor | Pass, two differently configured instances, duplication, undo/redo, closed-editor recall, offline and real-time audio | Pass, Low Contour gesture recorded, envelope saved/reopened and rendered with editor closed |
| VPRE-73 2.5 BUILD 264 | x64 | 6 parameters, stereo, original editor opens with lock overlay | User confirmed this installation is unactivated. Activation remains a prerequisite for processing acceptance | Activation prerequisite |
| LexHall 1.3.8 | x64 | Earlier inspection found 53 parameters and a 780 × 540 editor | Skipped at the user's request because an external file is unavailable | Skipped at the user's request |

SoundToys and Black Rooster versions come from PE resources. BBE has no PE version resource. Its runtime name and vendor are reported above. Commercial DLLs stay in the ignored local fixture directory and are excluded from the staged application.

All four retained sources generate through the staged GUI with PATH restricted to Windows System32. Their single-file outputs are under `out/gui-generated`. All four editors pass floating, FX-chain, close and reopen checks. Healthy conversions present the original editor. Error recovery presents the bridge diagnostic and Retry button.

The commercial state script processes a two-second stereo signal through three recalled instances. Decapitator's offline peak/RMS is 0.475793/0.249203 and real-time peak is 0.392551. Little AlterBoy's offline peak/RMS is 0.004161/0.000830 and real-time peak is 0.001628. BBE's offline peak/RMS is 0.549784/0.265887 and real-time peak is 0.549784. These establish audible processing and recall, with source-specific processing determining the waveform.

Original-editor mouse gestures change each vendor parameter in REAPER write mode. The saved envelope reopens with the same point count, then renders with the editor closed. Automation render peaks are 0.677158 for Decapitator, 0.199155 for Little AlterBoy and 0.336509 for BBE. The controlled fixture separately verifies expected automation plateau samples exactly.

## Automated suites

The final pre-review suites pass 101 x64 cases and 18 x86 cases in both Debug and Release. The x64 suite covers the generator, packaging/cache, VST3 COM entry points, bridging, audio/events, parameters, state and recovery. The x86 suite verifies the native VST2 ABI, inspection and editor lifecycle.

```powershell
pwsh -File scripts/Build.ps1 -Configuration Debug
ctest --test-dir out/build/x86 -C Debug --output-on-failure
ctest --test-dir out/build/x64 -C Debug --output-on-failure
pwsh -File scripts/Build.ps1 -Configuration Release
pwsh -File scripts/Verify.ps1 -Configuration Release
```

`Conversion.ConfiguredAssets` ensures each configuration uses its own fixtures. Release test results therefore exercise Release sources and helpers.

## Acceptance coverage

| Design check | Evidence and result |
| --- | --- |
| 1. Single-file scan, identity, category and parameters | REAPER lists source-derived identities under Nytera. Native factory tests construct processors before any editor and verify the complete parameter list. |
| 2. Coexisting conversions and project recall | Distinct Alpha/Beta conversions retain identities after project reopen. Commercial projects recall multiple independently configured instances. |
| 3. x86 and x64 sources produce audio | Decapitator and BBE x86, Little AlterBoy x64, and controlled sources pass real-time/offline checks. |
| 4. Original editor lifecycle and DPI | Four supplied editors pass floating/FX-chain/open/close/reopen. Native containment tests pass content scales 100%, 150% and 200%. Physical multi-monitor DPI changes remain untested. |
| 5. Automation recording and replay | Controlled x86 editor gesture renders exact expected plateaus. Three commercial original-editor gestures record and recall their source parameters. |
| 6. MIDI instrument timing | Controlled instrument renders exactly 12000 nonzero samples, from sample 12000 through 23999 at amplitude 0.5, with matching stereo channels. Real-time peak is 0.5. Native MIDI/SysEx tests cover both source architectures. |
| 7. Chunk and parameter/program state | Controlled x86 chunk and x64 program/parameter projects pass closed-editor two-instance recall, duplication, undo/redo and exact DSP plateaus. Commercial projects pass the same state workflow. |
| 8. Latency | Measured native impulse delay is 144 samples for a 64-sample maximum plus 16 source samples, and 1040 for a 512-sample maximum plus 16. REAPER compensates the effect impulse back to sample 12000 at amplitude 0.4. Decapitator reports 2110 total samples with a 1024-sample maximum. |
| 9. Blocks, rate, seeks, bypass and preparation | Native tests pass block lengths 0, 1, 63, 64, 127, 256 and 511, tempo changes within a quantum, stopped position, seeks, held-note release, bypass delay, float/double host buffers, repeated preparation and mode switches. |
| 10. Failure containment and recovery | Native cases cover crash/hang during snapshot, independent instances, retained snapshots, cancelled startup, malformed control, pending editor work and stalled offline teardown. Typical forced teardown is 150–210 ms. REAPER Recovery passes termination of the first matched helper, continued audible output from the second instance, Retry, restored Gain 0.25 and resumed first-instance audio. |
| 11. Relocation and automatic extraction | REAPER Relocation passes a renamed single file after removing its temporary original DLL, then passes again after moving its active cache entry aside. The exact cache entry reconstructs from the file, audio peaks at approximately 0.4 and project recall retains Gain 0.5. Native cases additionally cover corrupted cache repair, concurrent extraction, Unicode/spaces, traversal/reserved-name rejection and real directory junction rejection. |
| 12. Toolchain-free generation | Four retained commercial sources export through the staged application with PATH containing only System32. Export uses the shipped templates and helpers. |

## Audio and event details

Reported delay is `2 * nextPowerOfTwo(maximumHostBlock) + sourceLatency`. Bypass preserves that delay. The bridge carries float32 audio. Float64 host buffers convert through float32, and double-only source processing has a native conversion path.

Prepared bridge callbacks perform zero counted heap allocations. `Processor.LargeSysex` exercises consecutive 64 KiB and 1 MiB messages through actual VST3 processing, verifies bytes and sample offset, and inspects preallocated storage on both sides of JUCE's buffer swap. The pinned JUCE patch widens internal MIDI record lengths to 32 bits, handles raw SysEx and supports 4096 output events. Supported bridge bounds are 4096 MIDI events and 1 MiB MIDI bytes per quantum.

`Processor.ModeSwitch` changes offline, real-time and prefetch modes on one instance without preparing again. Prefetch permits ordered bounded worker waits while reporting real-time source processing level. `Processor.HostPrefetch` also verifies REAPER's published real-time-thread query, because background calls can carry the generic real-time process mode. Real-time device processing retains the nonblocking pipeline. [REAPER host extensions](https://www.reaper.fm/sdk/vst/vst_ext.php), [VST3 host interface](https://github.com/justinfrankel/reaper-sdk/blob/main/sdk/reaper_vst3_interfaces.h)

`Parameter.Resolution` gives VST3 two points in a 64-sample host block. The pinned JUCE adapter applies the last point to that block. Automation resolution is one host block. Source display text is cached off the audio thread for the current value. Arbitrary-value formatting and text entry use normalized fallback values.

## State and editor ordering

State carries a checksummed source binding, opaque bank/preset data when available, current program/name and reported parameters. Early restore runs before audible processing. Capture timeout preserves the last complete envelope. Tests cover 9000-parameter snapshots, legacy metadata, simultaneous capture, generation changes and a host requesting state reentrantly during automation.

The combined JUCE component/controller synchronizes immediately after restore. Source acknowledgements prevent older editor notifications or parameter polls from overwriting newer queued host values. Editor creation preserves the source program and scalar values if the source changes them during initialization.

REAPER acceptance yields separate message-loop turns between edits, undo/redo and save/render. A native stack trace showed REAPER can submit an older pending host parameter later when all those operations run in one Lua callback. The bridge honors subsequently delivered host automation. The UI-turn workflow passes controlled and commercial state checks.

## Reproduction and evidence

```powershell
pwsh -File scripts/Verify.ps1 -Configuration Release `
  -ReaperPath 'C:\Program Files\REAPER (x64)\reaper.exe' -AcceptanceScript Audio
```

The script also supports State, Automation, Editors, Recovery and Relocation. Close completed-render dialogs between projects. Automation requests a click in the controlled original editor. Recovery requests terminating the first instance's matched helper and using Retry. Editors expects the four local exports under `out/gui-generated`. Relocation removes a temporary source copy and renames its conversion before loading. To repeat cache reconstruction, close the dedicated profile, move that conversion's test-cache entry aside and launch Relocation again. The verification run preserved old cache entries under sibling names after automatic review rejected recursive deletion.

Commercial scripts are `tests/reaper/CommercialState.lua` and `CommercialAutomation.lua`. They require the corresponding local exports in the test profile, and the automation script expects gestures on Drive, Pitch and Low Contour during the marked recording interval. CommercialState creates its input WAV.

Local evidence lives under `out/reaper-acceptance` in `audio-result.txt`, `state-result.txt`, `automation-result.txt`, `commercial-state-result.txt`, `commercial-automation-result.txt`, `editors-result.txt`, `recovery-result.txt` and `relocation-result.txt`. Screenshots are under `out/inspection`. Build and suite logs are under `out`.
