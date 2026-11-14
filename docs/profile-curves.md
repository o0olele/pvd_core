# Profile duration curves in native scene time

`pvd::analysis::ProfileCurves` in `pvd/profile_curves.hpp` builds timing series
from an incrementally updated `pvd::profile::Index`. It has no PhysX, ImGui,
windowing, GPU or network dependency. `pvd::analysis` is the exported CMake target.

```cpp
pvd::analysis::ProfileCurves curves(session);
pvd::analysis::ProfileCurveRequest query;
query.source.scene_id = scene_id;
query.filter.zone = pvd::profile::ZoneKey{zone_id, generation};
query.filter.event_id = event_id;
query.metric = pvd::analysis::ProfileMetric::total_nanoseconds;
auto series = curves.sample(query);
pvd::analysis::write_profile_curve_csv(output, series);
```

## Counter-domain evidence

This implementation uses original CPU scope counters, never the event-group
receipt timestamp or the frame containing the flush packet. In the local
`D:/Code/PhysX-3.4-master` checkout:

- `PhysX_3.4/Source/PhysX/src/buffering/ScbScenePvdClient.cpp:875,933` calls
  `sendBeginFrame` / `sendEndFrame` for the scene.
- `PhysX_3.4/Source/PhysX/src/PvdMetaDataPvdBinding.cpp:673,783` implements those
  methods with `beginSection(scene,"frame")` / `endSection(scene,"frame")`.
- `PxShared/src/pvd/src/PxPvdDataStream.cpp:759,766` timestamps both section
  records with `Time::getCurrentCounterValue()`.
- `PxShared/src/pvd/src/PxProfileEventBuffer.h:161,166,183,188` uses that same
  `shdfnd::Time::getCurrentCounterValue()` for ordinary CPU start/stop events.
- `PxShared/src/foundation/src/windows/PsWindowsTime.cpp:71-74` implements that
  counter with `QueryPerformanceCounter`.
- `PxShared/src/pvd/src/PxPvdCommStreamEvents.h:313-314` writes the counter
  calibration, multiplying the foundation's tens-of-nanoseconds numerator by
  ten. Thus `nanoseconds = ticks * numerator / denominator`.

This is evidence for the SDK-native counter domain and common epoch. The header
frequency by itself cannot prove a common epoch. In particular,
`PxProfileZoneImpl.h:288` accepts arbitrary caller-supplied `atEvent` timestamps
without converting them. A producer using another clock must first convert to
the native counter domain. Such a capture violates this API's input contract;
the raw wire has no clock-domain tag with which the consumer could detect it.
GPU events and event-value counters are not CPU duration scopes.

Two appended metrics, `native_frame_ticks` and `native_frame_nanoseconds`, use
the independent [native FrameTiming API](frame-timing.md). They measure the
recorded frame section's wall-clock duration, require no CPU scope collection,
and retain exact rational nanoseconds/CSV. Their `frame_timing` member and
attribution distinguish them from the original inclusive CPU metrics below.

## Frame attribution

Each point uses a native scene frame section's exact half-open counter window
`[begin_timestamp, end_timestamp)`. It contains each selected **received,
completed CPU scope whose original end counter lies inside that window**.

A scope spanning several frames contributes its full inclusive duration to the
window in which it ends. It is neither clipped nor spread across intervening
frames. A stop exactly at the frame's end is excluded; it can belong to a next
window beginning at the same tick. Stops in gaps between frame sections belong
to neither window. A zero-duration scope still contributes one completed scope.

Totals include nested scopes and overlapping threads; they can exceed the
frame's wall-clock duration. They are observed inclusive duration, not CPU
utilization or exclusive work. A scene selects a **time window**, not ownership:
a global zone's work can appear in overlapping windows for two scenes. Select a
zone/event/thread/context explicitly when that ownership is known. Profile
packet stream IDs are not equated with scene-frame stream IDs.

`FrameSource::stream_id` filters native markers only. `SampleLocation::frame_index`
remains the zero-based ordinal in the complete scene timeline before this
optional stream filter, matching the viewer's seek API. The UI can display
`frame_index + 1`. Explicit `after_fetch` validation sections are rejected: those
narrow sections do not describe complete simulation-frame CPU windows.

Late flush packets may contain stops belonging to much older frame windows.
Updating and resampling revises those historical buckets. Previously returned
series own their values and remain unchanged. No packet is assigned wholesale
to its receipt frame.

## Coverage and incomplete data

The SDK stream does not carry a collection-coverage or final-flush watermark.
Sealing a file cannot prove that a producer flushed every buffered scope. The
request makes absence handling explicit:

- `observed_only` is the default. A nonempty point describes received completed
  scopes; an empty window is a `not_collected` gap, not a fabricated zero.
- `assume_continuous` asserts that matching CPU events were continuously
  collected for these windows. With an advertised enabled event or an observed
  matching source, an empty window becomes `no_matches`. Count/total are zero;
  minimum, maximum and mean are undefined and remain empty.
- An unknown/unadvertised selector remains `not_collected`, even under that
  assertion. Compile-time disabled event metadata alone is not coverage proof.

Every sample has an explicit state:

| State | Meaning |
| --- | --- |
| `observed` | At least one matching received completed scope. This does not independently prove runtime collection coverage. |
| `no_matches` | Explicit continuous-collection assumption, known source, zero matching completed scopes in this window. |
| `not_collected` | No matching observation and insufficient collection/source evidence. |
| `incomplete` | Open capture, relevant corrupt/unclosed/abandoned scope data, failed source, budget limit or cancellation. |
| `invalid_clock` | Backwards native frame bounds, or a nanosecond metric requested with zero calibration numerator/denominator. |

Live points are provisional because a future flush may revise any earlier
window. `ProfilePartial::gap` is the default for incomplete points;
`observed_subset` explicitly allows plotting the received subset, retaining
`incomplete` and `provisional` flags. A subset mean/minimum/maximum is not
necessarily a lower bound on its final value.

Packet damage cannot generally be localized in native time. Relevant critical
profile diagnostics conservatively mark all windows for that selected zone as
incomplete. Non-timing warnings such as missing display names do not invalidate
raw paired durations. Resource saturation or suppressed diagnostics compromise
all selections. Valid raw counters remain available when calibration is invalid;
only nanosecond metrics/time coordinates become unavailable.

`series.complete` describes whether the bounded query finished its work, not
whether runtime collection coverage is known. Inspect sample state, provisional
flags, coverage policy and diagnostics as well.

## Exact values, bounds and incremental use

Counts, frame tick bounds, minimum and maximum durations are native integers.
Inclusive sums use a portable internal 128-bit accumulator and expose exact
decimal `total_ticks`; multi-thread sums exceeding `uint64_t` do not wrap. A
mean-ticks `exact_value` is the unreduced rational `total_ticks/completed_count`.
`Numeric`/plot conversion may round large values. CSV keeps exact integer text,
that rational, the approximate plot value, raw frame counters, selector,
calibration numerator/denominator, analysis prefix and quality flags. It uses
RFC4180 quoting and classic decimal formatting without changing caller flags.

Default limits are 2000 output rows and 20 million visited work items.
`max_samples` supports 1 through 1,000,000; `max_work` must be positive. The
separate profile index has its own packet/event/name/open-scope limits.
`Resampling::uniform` selects original frame rows including first/last (one
row selects the first); it never interpolates or combines skipped windows.
`break_before` disconnects points separated by skipped scene ordinals.
`Resampling::reject` rejects an oversized query. Work limits and cancellation
return explicit partial results; raw aggregate columns describe visited data.

For live input, construct one `ProfileCurves`, serialize it with Session append,
and call `update(session)` only after the accepted prefix grows. The profile
index and native frame directory process only new records. Calls cannot rewind
or bind another Session. A finished Session finishes the index automatically;
manual `finish()` seals the received profile log. Repeating an identical final
update is harmless. The bound Session must outlive the analysis object.

Sampling scans the already decoded intervals under its work budget; it does not
reparse packets or seek/copy object snapshots. Query/refresh on demand or at a
bounded UI refresh rate, rather than resampling on every render frame. A late
flush makes cached earlier series stale even if their frame directory has not
changed. `source_event_count` identifies the analyzed received prefix.

## Real SDK oracle

`examples/physx34_profile_frames_capture.cpp` is separate from older producers.
It creates a small fixture scene record, emits genuine SDK native frame sections
and uses a real `PxProfileZone`. It does not simulate a physics world. Its
`atEvent` inputs come from QPC, the verified SDK counter domain.

It emits six frame windows and 17 scopes: a long scope crossing frames 0 to 2,
overlapping threads, a zero-duration scope, an empty window, a scope outside all
windows and an advertised event that never runs. One early packet contains the
long scope's start; every stop is deliberately flushed after the last frame.

The producer has no `pvd_core`/analysis dependency. Its SDK
`parseEventData<false>` callback records the independently decoded starts/stops.
Independent QPC calls immediately before/after the native section APIs bracket
their internal timestamps. It rejects ambiguous scope endpoints in those small
brackets instead of guessing their frame. These bounds and SDK-parsed intervals
produce 60 per-frame event/thread/context oracle rows.

Outputs from a supplied prefix are `_capture.pxd2`, `_scopes.csv`,
`_windows.csv`, `_expected.csv`. Run the analysis test as:

```text
pvd_profile_curves_tests capture.pxd2 expected.csv windows.csv scopes.csv
```

The test verifies exact SDK scope records, native timestamps inside the QPC
brackets, calibration, all 60 count/sum/min/max/mean/nanosecond buckets, long-scope
semantics and incremental late-flush revision of history. Synthetic tests also
cover endian decoding, multiple frame streams, zone pointer reuse, malformed and
unclosed data, invalid clocks, near-`UINT64_MAX` elapsed time, wide sums, CSV
precision, sample reduction, work limits and cancellation.

The repository CMake/CTest integration validates the standalone unit suite in
Debug static and Release shared builds. With `PVD_BUILD_PHYSX34_VALIDATION=ON`,
`physx34_profile_frames_generate` and `physx34_profile_curves` regenerate and
verify the SDK oracle without linking the SDK into the analysis library.
