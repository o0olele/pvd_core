# Native frame wall-clock timing

`pvd/frame_timing.hpp` is part of the UI/SDK-independent `pvd::analysis` module.
It samples accepted, completed `FrameMarker` sections directly, without reading
CPU scopes or copying object snapshots.
Calls are synchronous; the caller serializes them with Session append and keeps
the Session unchanged during sampling and its callbacks.

```cpp
pvd::analysis::FrameTimingRequest request;
request.source.scene_id = scene_id;
request.metric = pvd::analysis::FrameTimingMetric::nanoseconds;
auto series = pvd::analysis::sample_frame_timing(session, request);
pvd::analysis::write_frame_timing_csv(output, series);
```

The duration is exactly `frame.end_timestamp - frame.begin_timestamp`. It is the
wall-clock span of the SDK's PVD instrumentation section, which can include SDK
scheduling/debug/transport overhead; it is not exclusive CPU work, simulation dt,
time between received packets, an after-fetch interval, or playback FPS.

The local PhysX 3.4 source sends begin/end frame sections in
`ScbScenePvdClient.cpp:875,933`, through `PvdMetaDataPvdBinding.cpp:673,783`.
`PxPvdDataStream.cpp:759,766` obtains their native counters from
`Time::getCurrentCounterValue()`. The stream header stores the calibration in
nanoseconds per tick (`PxPvdCommStreamEvents.h:313-314`), so the conversion is
`duration_ticks * timestamp_numerator / timestamp_denominator`. Native EndSection
still precedes the dynamic pose update; choosing this duration does not change
the existing frame-state sampling semantics. See [profile-curves.md](profile-curves.md)
for the full native counter-domain evidence.

Raw ticks use uint64 throughout. Nanoseconds retain an exact reduced rational:
the numerator uses portable 128-bit multiplication and decimal formatting, and
the denominator is uint64. `exact_nanoseconds` is an integer string when exact,
or a fraction such as `35/3`. `Numeric`/plot values may round; CSV always includes
the exact numerator, denominator, raw begin/end/duration ticks and calibration.
Caller stream formatting (for example hexadecimal output) cannot change the CSV
integer representation. Absolute/elapsed seconds are approximate graph
coordinates; elapsed time subtracts the original integer counters before any
floating conversion, preserving adjacent counters near UINT64_MAX.

Zero duration is a valid zero. Backwards begin/end counters produce an explicit
`backwards_clock` gap. Missing/zero calibration produces an
`invalid_calibration` gap for nanosecond metrics while preserving valid raw ticks.
Native tick metrics remain usable with `calibration_valid=false`. Explicit
after-fetch sources are rejected, rather than silently substituted for native
frames. Unclosed frames have no completed FrameMarker and are excluded; no zero
or invented provisional duration is appended. Closed frame durations do not
change because a CPU profile packet arrives late.

Requests support scene and optional stream selection, frame range and stride.
`frame_index` keeps the original scene ordinal before stream filtering. An
accepted `event_count` prefix excludes later EndSection records. Defaults are
2000 output points and 20 million visited markers; cancellation and work limits
return explicit partial results. Uniform resampling selects original points,
including gap rows, without interpolation. Skipped source ordinals set
`break_before`. UI frame labels are one-based; the public API/CSV source index
stays zero-based.

The Event curves panel offers **Frame time (native ticks)** and **Frame time
(native ns)** in the Metric selector. These modes do not require any advertised
profile zone and hide the inapplicable CPU filters/coverage controls. They retain
the existing legend, frame/time axis, wheel zoom, drag pan, point seek, 16-curve
and 2000-row limits, persistent source binding and optional 1 Hz refresh.
`ProfileCurvePanel::add_frame_timing` is also available for host scripting. Point
seek switches an after-fetch viewport back to the actual source native frame.

For API compatibility, the two metrics are appended to `ProfileMetric`; the
original nine CPU metric values keep their values and behavior.
`ProfileCurves::sample` adapts frame timing to the existing plot model and retains
the exact `FrameTimingSeries` in `frame_timing`. CSV export dispatches to the
frame-specific schema, avoiding meaningless CPU scope/count/coverage columns.
CPU filters supplied to this adapter are explicitly ignored with a diagnostic.

Verification:

- `frame_timing_tests.cpp` has independent known integer/fractional oracles,
  including `(2^64-1)^2 = 340282366920938463426481119284349108225`, zero/backwards
  frames, bad calibration, scene/stream ordinals, prefixes, budgets,
  cancellation, resampling and exact CSV despite caller hex formatting.
- Optional arguments are a real SDK capture and its `_windows.csv`. The existing
  PhysX 3.4 profile-frame producer brackets each SDK begin/end call with separate
  QPC reads. Six independently bracketed native windows verify the timestamps
  and duration bounds without using the analyzer to generate expectations.
- `profile_curve_panel_tests.cpp` actually clicks the Metric dropdown and Add,
  native plot points and Export CSV at 1060/420 widths. A capture with no CPU zones
  produces the known 5/10/20-tick series; exact `35/3` nanoseconds survives the
  real CSV callback. Existing CPU curve tests continue to run.
