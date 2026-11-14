# Numeric curves and state differences

`pvd/curves.hpp` is part of the independent `pvd::analysis` module. It depends on the core Session model, without PhysX, GPU, platform-window or ImGui dependencies. Its functions are synchronous and not internally synchronized. Keep the queried Session unchanged during a call; use an immutable loaded session or external synchronization when capturing live data.

## Property curves

```cpp
pvd::analysis::CurveRequest request;
request.source.scene_id = scene_id;
request.object = {actor_id, actor_generation};
request.property = "GlobalPose";
request.component = "position.y";
auto series = pvd::analysis::sample_curve(session, request);
```

Object identity includes the original 64-bit ID and its generation. A destroyed object produces an `object_absent` gap. If that ID later belongs to another generation, the old series produces `generation_mismatch`, rather than borrowing the new object's values. Properties not transmitted at a selected point are `property_missing`; missing array items are `item_missing`. None of these conditions becomes a numeric zero.

The property name is exact and separate from the numeric component. In particular, the reflected property `Scale.Scale` is a full property name; its `y` component is supplied separately. `item_index` chooses one original array item and defaults to zero. There is no implicit array averaging.

Supported numeric source layouts:

| Incoming type | Component |
| --- | --- |
| PvdU8/U16/U32/U64, PvdI8/I16/I32/I64, PvdBool, PvdF32/F64 | empty or `value` |
| PxVec2, PxVec3, PxVec4, PxQuat | `x`, `y`, `z`, `w`, as applicable |
| PxTransform | `position.x/y/z`, `rotation.x/y/z/w`; optional `Transform.` or `PxTransform.` prefix |
| PxBounds3 | `minimum.x/y/z`, `maximum.x/y/z` |
| PxMat33, PxMat44 | `m[row][column]`, zero-based; SDK storage is column-major |
| U32Array4 | `0`, `1`, `2`, `3` |

Source byte order is interpreted explicitly. Source pointer width is never confused with the host width. ObjectRef, VoidPtr, normalized reference lists, String and StringHandle are deliberately not numeric, even if their underlying bytes can be read as an integer. Unknown custom layouts remain unsupported gaps. NaN/infinity, malformed payloads, invalid booleans and invalid components have explicit gap codes. This analysis does not normalize a quaternion or repair a simulation value before plotting it.

`Numeric` preserves unsigned 64-bit, signed 64-bit and double alternatives. `plot_value()` permits a double representation for a graph, which may round large integers; the original variant and CSV preserve exact integer values.

## Sampling and time

Native sampling uses each scene's completed `EndSection("frame")` state. It does not describe post-simulation state: PhysX 3.4 emits this marker before sending dynamic-actor updates. `Sampling::after_fetch` requires an explicit matching section, defaulting to the fixture/application label `validation_after_fetch`. Another explicit label can be supplied; there is no guessed post-fetch offset.

Frame ordinals are in the selected scene/mode timeline, in capture order, before optional stream filtering. This matches the viewer's per-scene timeline and permits click-to-seek using `CurveSample.source.frame_index`. A stream filter may therefore leave gaps in those ordinals. The same scene across multiple streams is not sorted or deduplicated by timestamp.

`first_frame` is inclusive, `end_frame` exclusive, and `stride` is anchored at `first_frame`. A zero stride, inverted range, empty property, invalid mode or empty explicit-section label is rejected. An empty valid selection returns an empty series.

Every row retains raw section-end counter ticks and its exact global event prefix. The SDK initialization header already converts its frequency numerator to nanoseconds per native counter tick: `PxPvdCommStreamEvents.h::StreamInitialization` multiplies `Time::getCounterFrequency().mNumerator` by ten. Thus seconds are `ticks * numerator / denominator * 1e-9`. There is no assumption that ticks are already nanoseconds or simulation time. Missing calibration retains ticks with null seconds and a diagnostic. Elapsed seconds subtract the first retained row's raw tick before conversion, reducing large absolute-clock rounding. Backwards ticks remain visible as negative elapsed values; capture order is retained.

## Limits, resampling and cancellation

The default maximum output is 10,000 samples. `Resampling::reject` throws before snapshot queries if the eligible row count exceeds this budget. `uniform` deterministically selects original rows across the eligible range, including both endpoints when the budget is at least two. A budget of one keeps the first row. It never interpolates, fills a gap or averages values.

`break_before` is true when unsampled scene-timeline rows lie between consecutive retained points, whether because of stride, stream filtering or uniform selection. A graph must break a line there, and at every null value, instead of implying observed continuity across omitted data. `resampled` and `candidate_samples` describe the reduction. `complete` means the requested selection policy completed, not that every original frame was retained.

Enumeration uses two passes over existing frame/section indexes and stores only the bounded output locations, without making a per-frame history-sized sample array. Snapshot replay is performed only for retained locations. The cancellation callback is polled during matching-frame enumeration and before sample snapshots. Cancellation returns the valid rows already completed with `cancelled=true, complete=false`. If cancellation happens while counting candidates, that count is partial. The progress callback reports completed retained rows and their selected total. Callers that need interactive cancellation should execute analysis on a worker with a stable Session view.

## CSV

`write_curve_csv(stream, series)` streams RFC4180 rows, including quoted property/component names, raw ticks, frame/event locations, integer object identity, gap reason, continuity break, sampling mode and completion/resampling flags. Unsigned and signed 64-bit values are formatted directly as decimal integers; floating values use `max_digits10` and the classic locale. Missing values are empty cells, accompanied by the gap column. File selection, UTF-8 path handling and overwrite policy belong to the caller.

## State differences

```cpp
auto changes = pvd::analysis::diff_events(session, before_prefix, after_prefix);
auto frames = pvd::analysis::diff_frames(
    session, {source, before_frame}, {source, after_frame});
```

Endpoints may be in either time order. `diff_frames` accepts different scene/stream/sampling sources at its two endpoints, so native and explicitly post-fetch states can be compared directly. Unknown frame locations or event prefixes beyond the Session throw clear range errors. `diff_snapshots` also accepts standalone core snapshots.

Diffs compare object lifetimes and class, pickable/top-level flags, optional color, property presence, incoming type/encoding/count, immutable raw payload bytes, decoded text and normalized references. Equal bytes held in different buffers are unchanged. ID reuse yields a destroyed row for the old generation and a created row for the new generation. Camera and explicit origin-shift changes are included. Raw property values are preserved; numerical equality or tolerance is not substituted for byte equality.

Lifecycle rows retain attributes by default. Set `include_lifecycle_properties` to retain their properties as well. Changed-property rows share immutable byte buffers, while their text and reference vectors remain independent values. The result does not fabricate historical metadata-definition differences, which the core Snapshot does not contain.

Object/property limits bound visited union objects/properties, including unchanged values, rather than only output changes. The default budgets are 10,000 objects, 100,000 properties, 10,000 auxiliary changes and 64 MiB of logically retained property data. Reaching a budget returns a valid partial result with `truncated=true, complete=false` and a diagnostic. Cancellation similarly marks a partial result. Partial output must not be presented as proof that the unvisited state is equal.

## Verification

`curves_tests` covers integer extremes and CSV precision, scalar/vector/quaternion/transform/matrix components, arrays, late properties, destruction/ID reuse gaps, explicit post-fetch sampling, stream isolation and viewer-compatible ordinals, big-endian/32-bit-pointer sources, cancellation, resampling and continuity breaks. Diff checks include forward/reverse lifetimes, byte/text/reference changes, display attributes, cameras/origin shifts and resource budgets. An optional raw capture argument validates real SDK `GlobalPose.position.y` rows against core snapshots and verifies property changes across actual frames.
