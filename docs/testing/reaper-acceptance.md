# REAPER compatibility results

## Environment

Initial host checks use a locally extracted portable REAPER 7.85 x64 with a dedicated profile under `out/reaper-test`. Its child environment redirects default plugin directories to that profile. Original user profiles and installed plugin directories are outside the test profile.

## Factory feasibility

The native factory suite passes distinct generated identities, matching controller IDs, parameters available before editor creation, and stable identity after file renaming. REAPER scans two physical single-file outputs and lists Nytera Test Alpha and Nytera Test Beta under Nytera. The first Lua run loaded both and confirmed the original Gain parameter. The Lua acceptance script passes project save/reopen with both identities preserved.

## Source plugin checks

| Source | Architecture | Inspection | Original editor | Audio and recall |
| --- | --- | --- | --- | --- |
| Decapitator 5.0.1 | x86 | Pass, 12 parameters, 2 in / 2 out | Reports 870 x 423 | Pending |
| LexHall 1.3.8 | x64 | Pass, 53 parameters, 2 in / 2 out | Reports 780 x 540 | Pending |
| Little AlterBoy 5.0.1 | x64 | Pass, 7 parameters, 2 in / 2 out | Reports 870 x 283 | Pending |
| VPRE-73 2.5 BUILD 264 | x64 | Pass, 6 parameters, 2 in / 2 out | Reports 800 x 196 | Pending |

Versions come from PE resources. All four sources have now loaded and completed runtime metadata inspection through matching helpers. Editor dimensions are source-reported. Interaction, processing, and recall checks follow in later tasks. LexHall exceeded the initial 15-second inspection deadline on its first attempt and succeeded when allowed to finish. A timed successful initialization took 33.58 seconds, so startup and inspection use a 60-second deadline.
