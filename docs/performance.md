# Replay performance baseline

`pvd_benchmark capture.pxd2 [seek_samples=256] [--no-checkpoints] [--load-only]` measures a normal core file load,
deterministic random native-frame snapshots, and reverse snapshots. It prints
JSON with input bytes/events/frames, load wall time, throughput and seek
distributions, checkpoint count and estimated owned checkpoint bytes. On Windows
it also reports current/peak working set and private commit after load and after
seeks; other platforms report zero for these unavailable counters. `--load-only`
skips seeks and `--no-checkpoints` provides a replay-index comparison. The process performs no
rendering, ImGui work or simulation. This is a diagnostic tool, not a universal
performance threshold.

The 2026-11-15 local Release shared build measured this real SDK fixture:

```powershell
./build-physx34/Debug/pvd_physx34_capture.exe ./build-physx34/fixtures/long 3000
./build-shared/Release/pvd_benchmark.exe ./build-physx34/fixtures/long_capture.pxd2 256
```

The capture contains 4,448,937 bytes, 37,706 events and 3,000 native frames,
with only a few rigid actors. Both measurements used the same file, default
Session checkpoints, sample sequence and checksum 56303.

| Measurement | Full property-message history | Changed-property replay history |
| --- | ---: | ---: |
| Load wall time | 1261.90 ms | 587.35 ms |
| Process peak working set | 407,875,584 bytes | 26,034,176 bytes |
| Random seek median / p95 | 4.999 / 9.631 ms | 0.027 / 0.059 ms |
| Reverse seek median / p95 | 8.150 / 11.138 ms | 0.026 / 0.054 ms |

These are individual local runs with normal filesystem caching. They establish
a long-recording regression baseline, not many-object, mesh-heavy, cold-storage
or interactive GPU performance. The JSON artifacts are local build outputs
`build-shared/benchmark-long.json` and `benchmark-long-delta.json`.

The replay index now omits assignments whose complete typed value is unchanged
and compacts each atomic message to its final changed fields. Equality includes
incoming type, item count, byte order, pointer width, opaque state, decoded text,
resolved references and exact raw bytes. Distinct float bit patterns remain
distinct; repeated arguments use their final value. Original source events,
event-prefix positions, frame markers and every analysis-channel packet remain
present, including identical repeated packets. Raw save uses the original
events, so this optimization does not compress or alter the recorded stream.
File parsing also transfers decoded events rather than deep-copying them.

Validation covers SDK forward/reverse/no-checkpoint snapshots, raw/crop
roundtrips, memory/profile/deformables/RepX oracles, repeated message arguments,
negative zero, incoming-type changes, retained snapshots and identical repeated
channel packets. Clang static allocation-fault verification passed 157 fault
points; Debug static and Release shared Session tests also passed.

A second SDK workload exercises 128 simultaneous rigid actors (96 dynamic,
32 static) and 48 frames, with boxes, spheres and capsules and one shared
material. `physx34_stress` compares all 12,288 actor/frame poses and types in
forward/reverse order with independent SDK getters, and checks first/last
geometry extraction. The 751,551-byte / 8,922-event recording measured a
53.90 ms Release load, 23,687,168-byte peak working set, 1.252 ms random-seek
p95 (128 samples) and 1.048 ms reverse-seek p95 (48 samples). This is a modest
many-actor workload, still not a mesh-heavy production-scene benchmark.

Regenerate with `pvd_physx34_stress_capture PREFIX [actors=128] [frames=48]`;
supported ranges are 16–2048 actors and 16–1024 frames. Its `_expected.csv`
records SDK poses before each simulate call, matching the documented native
EndFrame boundary. The optional `physx34_stress_generate` / `physx34_stress`
CTest fixture uses the default workload; it does not alter the SDK tree.

## Large reference-array regression (0.10)

A local production recording exposed a different workload: 11,387,121 input
bytes, 106,633 events, 25,982 live objects and 25,978 reference mutations. One
array receives 25,976 successive appends. The file contains material/resource
initialization, with zero native frames, rigid actors or meshes. Loading must
still finish and expose those resources; this file cannot validate 3D playback.
The private source recording and generated copies are excluded from version
control.

The old replay index retained the complete raw bytes and resolved IDs after
every mutation. Intermediate arrays contain 337,389,276 IDs in total, requiring
over 5.39 GB for their logical payloads alone, before vector capacities and other
state. Unbounded structural checkpoint copies added further memory. A controlled
Release core-only run exceeded 1.5 GiB private memory in 1.86 seconds before load
completed and was stopped; this reproduces the growth without UI or GPU work.

The fix records reference deltas and materializes canonical wire bytes only at
checkpoints, absolute-assignment comparisons, and public snapshot boundaries.
Appending is amortized constant time; removal retains first-match/order
semantics. A default 128 MiB estimated checkpoint-structure budget evicts old
copies before creating new ones. Immutable payloads remain shared. The budget
does not cap the entire process or include the source event history.

| Same recording, Release core | Default checkpoints | Checkpoints disabled |
| --- | ---: | ---: |
| Load wall time | 2,050 ms | 591 ms |
| Private bytes immediately after load | 429,031,424 | 268,132,352 |
| Process peak commit | 505,651,200 | 369,729,536 |
| Retained checkpoints | 2 | 0 |
| Estimated checkpoint structures | 132,883,254 bytes | 0 bytes |

These are individual warm-filesystem local runs. Peak commit differs from peak
working set, and both differ from a full viewer. The viewer also owns its current
snapshot, analysis/query state and graphics resources. A measured second issue
was paused hierarchy rendering repeatedly scanning every material for an actor
tree. Caching by snapshot prefix, scene and session metadata revision reduces
paused panel CPU time from median 13.47 ms to 0.031 ms on this input. Applying
unchanged view options also preserves the published frame instead of copying it.

The installed 0.10 Release viewer was then run from a different working directory
against the original recording using normal asynchronous `--capture` loading,
`--hidden --no-settings --screenshot`, and a fresh screenshot path for each run.
Each owned test process was monitored at 50 ms intervals with a 1.5 GiB private
memory stop threshold and a 30-second deadline, observed for four more seconds
after its loaded screenshot, then stopped. Both paths completed:

| Installed full viewer | Hardware D3D11 | WARP D3D11 |
| --- | ---: | ---: |
| Loaded screenshot ready | 2.814 s | 2.588 s |
| OS peak process commit | 700,817,408 bytes | 676,593,664 bytes |
| Sampled peak private bytes | 692,727,808 | 675,938,304 |
| Sampled peak working set | 679,129,088 | 667,594,752 |

The complete EOF snapshot (all typed fields, raw bytes and resolved references)
matches with and without checkpoints. Saving and reopening preserves every
event and the complete snapshot; the saved file is also byte-for-byte identical
to the input. The original file was not modified. These checks run in separate
bounded sessions; a no-frame recording is not passed to a smoke test that
requires two replay frames.

`session_refs` uses synthetic 32/64-bit, little/big-endian streams for absolute
writes, chunks/messages, duplicates/removal, ID reuse, snapshot independence,
checkpoint eviction and failed-append recovery. In static builds an allocation
volume guard compares 1,000 and 2,000 appends before attempting 40,000 appends,
catching quadratic retention without exhausting memory. Shared builds run the
same semantic checks. Existing SDK raw/crop/coordinate tests verify unchanged
save and playback behavior.

Final 0.10 validation: Debug static with the real PhysX 3.4 SDK passed 158/158
CTest tests; Release shared passed 54/54. The installed external CMake consumer
was rebuilt and exercised core, transport, render, analysis and RepX successfully.
The SDK/UI-off core-only Release build also passed 6/6 tests.
`viewer_cache` covers exact no-op publication identity, signed-zero settings,
forward/reverse names, reused IDs, scene changes at the same EOF prefix, paused
live metadata changes and source replacement.
