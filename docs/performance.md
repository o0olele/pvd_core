# Replay performance baseline

`pvd_benchmark capture.pxd2 [seek_samples=256]` measures a normal core file load,
deterministic random native-frame snapshots, and reverse snapshots. It prints
JSON with input bytes/events/frames, load wall time, throughput and seek
distributions. On Windows it also reports process peak working set; other
platforms report zero for this unavailable counter. The process performs no
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
