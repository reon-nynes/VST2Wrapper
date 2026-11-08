# REAPER compatibility results

## Environment

Initial host checks use a locally extracted portable REAPER 7.85 x64 with a dedicated profile under `out/reaper-test`. Its child environment redirects default plugin directories to that profile. Original user profiles and installed plugin directories are outside the test profile.

## Factory feasibility

The native factory suite passes distinct generated identities, matching controller IDs, parameters available before editor creation, and stable identity after file renaming. REAPER scans two physical single-file outputs and lists Nytera Test Alpha and Nytera Test Beta under Nytera. The first Lua run loaded both and confirmed the original Gain parameter. The Lua acceptance script passes project save/reopen with both identities preserved.

## Source plugin checks

| Source | Architecture | Inspection | Original editor | Audio and recall |
| --- | --- | --- | --- | --- |
| Decapitator 5.0.1 | x86 | Pending | Pending | Pending |
| LexHall 1.3.8 | x64 | Pending | Pending | Pending |
| Little AlterBoy 5.0.1 | x64 | Pending | Pending | Pending |
| VPRE-73 2.5 BUILD 264 | x64 | Pending | Pending | Pending |

Recorded source names and versions currently come from PE headers and version resources. The implementation will record runtime observations as each path becomes available.
