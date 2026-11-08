# REAPER compatibility results

## Environment

Initial host checks use a locally extracted portable REAPER 7.85 x64 with a dedicated profile under `out/reaper-test`. Its child environment redirects default plugin directories to that profile. Original user profiles and installed plugin directories are outside the test profile.

## Factory feasibility

The native factory suite passes distinct generated identities, matching controller IDs, parameters available before editor creation, and stable identity after file renaming. REAPER scans two physical single-file outputs and lists Nytera Test Alpha and Nytera Test Beta under Nytera. The first Lua run loaded both and confirmed the original Gain parameter. The Lua acceptance script passes project save/reopen with both identities preserved.

## Source plugin checks

| Source | Architecture | Inspection | Original editor | Audio and recall |
| --- | --- | --- | --- | --- |
| Decapitator 5.0.1 | x86 | Pass, 12 parameters, 2 in / 2 out | Original editor visually verified inside REAPER, float/chain/reopen pass | Pending |
| LexHall 1.3.8 | x64 | Pass, 53 parameters, 2 in / 2 out | Reports 780 x 540 | Pending |
| Little AlterBoy 5.0.1 | x64 | Pass, 7 parameters, 2 in / 2 out | Reports 870 x 283 | Pending |
| VPRE-73 2.5 BUILD 264 | x64 | Pass, 6 parameters, 2 in / 2 out | Direct bridge display and reopen pass, lock overlay visible | Pending |
| BBE D82 Sonic Maximizer | x86 | Pass, 5 parameters, 2 in / 2 out | Direct bridge open/close/reopen pass, reports 800 x 160 | Pending |

Versions come from PE resources. All four sources have now loaded and completed runtime metadata inspection through matching helpers. Editor dimensions are source-reported. Interaction, processing, and recall checks follow in later tasks. LexHall exceeded the initial 15-second inspection deadline on its first attempt and succeeded when allowed to finish. A timed successful initialization took 33.58 seconds, so startup and inspection use a 60-second deadline.

The cross-process editor regression initially reproduced a ten-second timeout. Dispatching synchronous sent window messages during control waits resolves the parent/child creation wait cycle. Native x86/x64 reopen, resize, owner teardown, and cross-bitness parenting checks pass. Direct source screenshots are recorded under `out/inspection`. The REAPER Lua check passes floating and FX-chain editor open/close/reopen for the embedded Decapitator and VPRE-73 conversions. `out/inspection/REAPER-Decapitator-editor.png` visually confirms the complete original Decapitator editor inside REAPER's normal frame. The user confirmed VPRE-73 is unactivated in this account, so its locked editor is an activation prerequisite for meaningful source audio tests.

## Embedded payload checks

The seven package cases pass relocation with the original fixture DLL removed, real helper startup from the extracted payload, concurrent extraction, corrupted-cache repair, Unicode export/cache/helper paths, traversal/reserved-name rejection, transactional failed export, and rejection of a real Windows directory junction as cache root. All five supplied demo DLLs have been embedded into dedicated single-file test outputs. The additional BBE DLL scans in REAPER as D82 Sonic Maximizer under Nytera. Commercial-source processing and state recall remain pending.

## Audio and event translation

The current debug suite passes 47 x64 cases and 13 x86 native cases. Prepared bridge callbacks perform zero counted heap allocations. Tests cover block lengths 0, 1, 63, 64, 127, 256, and 511, transport validity, stopped position, tempo changes within a quantum, seeks, held-note release, bypass latency, double-input conversion, suspend/resume, and ordered offline completion. MIDI and a 64 KiB SysEx message round-trip through both helper architectures.

The pipeline reports `2 * nextPowerOfTwo(maximumBlock) + sourceLatency`. With a 64-sample maximum and a 16-sample source delay, the measured impulse occurs at sample 144. With a 512-sample maximum it occurs at sample 1040. Bypass preserves the same reported delay. Both normal VST3 processing entry points pass native COM lifecycle tests. Float64 host buffers convert through the float32 bridge, so the transport's precision is float32.

`tests/reaper/Audio.lua` passes in portable REAPER 7.85 x64 with its dedicated Dummy Audio profile. The embedded x86 delay fixture renders an input impulse at sample 12000 with amplitude 0.4 after REAPER compensation. The x64 MIDI fixture renders exactly 12000 nonzero samples from 12000 through 23999 at amplitude 0.5. Both stereo channels match. Real-time playback meters observe the expected 0.4 and 0.5 peaks. The script verifies every output sample of both 48000-frame offline WAV files. Results are recorded in `out/reaper-test/audio-result.txt`.
