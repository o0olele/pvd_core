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

## Interactive scene replay (0.11)

A second private recording contains 25,853,397 bytes, 283,917 events and 2,177
native frames. Its EOF state contains 44,870 objects, including 41,454 materials,
225 rigid actors, 1,462 shapes and 256 triangle-mesh resources. It exercises real
playback and camera interaction, beyond the initialization-only recording above.
Neither recording, screenshots nor generated roundtrip copies are tracked.

Three independent costs were measured in the original viewer:

- Each playback update spent roughly 1.8–2.1 seconds reconstructing full states,
  including repeated full snapshots for scene-query publications. A single full
  snapshot copied approximately 400,000 property nodes and cost about 100 ms.
- The paused first scene used 410 CPU triangle buffers against a 256-entry GPU
  LRU. Orbiting uploaded roughly 406 meshes again on every frame. Hardware render
  completion p50/p95 was 30.90/33.33 ms, even though the geometry itself was small.
- An expanded class browser rebuilt over 40,000 static material labels whenever
  a few temporary query objects appeared or disappeared. Its per-playback-frame
  panel time was approximately 45–50 ms.

The viewer now publishes complete immutable instance views with flat shared
indexes, reconstructs changed properties by version, and uses exact selected
object queries for scene-query publications. A bounded derived cache also
reuses complete reference-property versions without recording every intermediate
array. Materials remain accessible in the full view and inspector. Spatial
readers consume a shared subset directly, and legacy mutable Snapshot APIs
remain available for explicit materialization. Origin-shift prefixes retain
their exact full-state fallback.

CPU mesh caches can reuse resources still owned by a returned frame without
increasing their bounded strong LRU. GPU eviction preserves the current visible
working set, evicting inactive resources first. Class browsing updates affected
groups without rebuilding unrelated material labels, matches or pages. The
viewer uses indexed views instead of legacy full-state checkpoints and imports
live deltas at display cadence through nonblocking source polling.

A separate MSVC-specific spike came from replay-history growth. Each old
Operation embedded an InstanceState containing a std::map; its move constructor
could allocate, so vector growth copied existing assignments. A 4,096-operation
relocation test made 8,193 allocations / 11.24 MB of allocations. Creation
prototypes are now separately owned only by create operations, and Operation is
move-only with a compile-time noexcept-move check. The same relocation requires
one allocation / 1.64 MB for the new contiguous buffer, preserving existing
operation indices and source events.

Reproduce the complete viewer pipeline without a window:

```powershell
./install-shared/bin/pvd_viewer_benchmark.exe capture.pxd2 --samples 32
./install-shared/bin/pvd_viewer_benchmark.exe capture.pxd2 --samples 32 --browse
./install-shared/bin/pvd_viewer_benchmark.exe capture.pxd2 --samples 16 --warp
```

The Windows-only tool loads a normal Document and renders through the production
D3D11 renderer. It drives orbit, pan, zoom, forward/reverse playback and random
seeks, and paints the actual ImGui hierarchy, inspector and timeline. `--browse`
opens the standard PhysX classes. Output contains counts, memory and p50/p95/max
stage times, not object names or property values. Render-submit time is CPU
submission; render-complete time waits for an actual GPU completion query.
These figures exclude native input dispatch, presentation/vsync and the OS
compositor, so they are not a claim about measured end-to-end input latency.

Final local Release measurements used an idle system, the same original file,
64 samples for the default hardware path, 32 for expanded browsing and 16 for
WARP. The 960×640 viewport used normal layers; no geometry or analysis data was
discarded to obtain these times:

| Stage | p50 | p95 |
| --- | ---: | ---: |
| Hardware camera render, GPU complete | 0.831 ms | 1.011 ms |
| Hardware forward model update | 14.013 ms | 16.905 ms |
| Hardware reverse model update | 16.912 ms | 20.255 ms |
| Hardware random seek model update | 14.607 ms | 17.117 ms |
| Default forward panels | 3.487 ms | 5.595 ms |
| Expanded class-browser forward panels | 7.059 ms | 8.337 ms |
| WARP camera render, GPU complete | 8.679 ms | 11.326 ms |

The hardware benchmark loaded in 1.575 seconds, peaked at 755,253,248 bytes of
process commit, and recorded zero mesh uploads during camera movement. The
installed full Win32 viewer's normal asynchronous loading path produced its
loaded screenshot in 2.137 seconds with 829,874,176-byte peak commit. This latter
test observed four further seconds and then stopped only its own hidden process.

Live validation sent the original bytes over loopback TCP, with initialization
sent first and all remaining native-frame groups paced at 60 Hz. The normal
LiveSession used 16 ms polling, 2,048-event / 2 MiB limits and no legacy
checkpoints. While receiving and recording, the same thread continuously drove
orbit/pan/zoom and actual 1280×720 hardware rendering. The 2 GiB-bounded run
accepted all 283,917 events / 2,177 frames and recorded all 25,853,397 bytes.
The output was byte-for-byte identical and the temporary recording was removed.

| Live stage | p50 | p95 | Maximum |
| --- | ---: | ---: | ---: |
| Initial import poll | 10.239 ms | 28.530 ms | 32.374 ms |
| Steady import poll | 16.946 ms | 20.520 ms | 34.388 ms |
| Camera render, GPU complete | 1.499 ms | 2.011 ms | 36.137 ms |
| Whole-run loop interval | 17.508 ms | 23.523 ms | 228.567 ms |

The earlier steady import spike of 170.778 ms disappeared after noexcept history
relocation; no final poll exceeded 40 ms. Peak commit dropped from 1,723,142,144
to 1,404,579,840 bytes. The run published 1,794 display updates, approximately
49 per second during paced simulation. It does not establish stable 60 FPS.
The loop statistic includes initialization, scheduling and diagnostic work
outside the measured poll/render scopes; its maximum is retained rather than
attributed to a component without evidence. This live probe does not paint ImGui
panels; their costs are measured separately above. The probe requested a 1 ms
Windows timer interval and TCP_NODELAY for reproducible pacing.

Regression coverage includes immutable-view/full-state equivalence, exact
selected queries, reverse/ID reuse, 32/64-bit little/big-endian encodings,
reference resets/removal, native origin shifts, concurrent const queries,
allocation failures, cache budgets and long class names. A static allocation
guard verifies that a 4,099-object view update copies a flat index instead of
allocating one map node per object. Actual WARP pixel tests cover more than 256
visible meshes, opacity, wireframe, picking, invalidation and cache clearing.
Derived-data truth checks run through both Snapshot and SnapshotView readers.

Final validation passed 161/161 Debug tests with the real PhysX 3.4 SDK, 57/57
Release shared tests, and 7/7 SDK/UI-off core-only tests. Installed external
consumers linked and ran both the full shared package and the core-only static
package, exercising the new view/projection, selected-object, nonblocking poll
and direct-view rendering entry points. The private source recording's size and
last-write time were unchanged.
