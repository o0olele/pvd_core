# Native-frame memory receipt statistics

`pvd::memory::Index::window_statistics` adds interval activity to the existing
prefix/history APIs. It belongs to the SDK/UI-independent `pvd::analysis`
library. Existing `summary_at_event`, `snapshot_at_event`, history, grouping and
CSV behavior is unchanged.

```cpp
pvd::memory::Index index(session);
const auto& frame = session.frames().at(2);
auto window = pvd::memory::native_frame_event_window(session, frame);
auto stats = index.window_statistics(window);
// Optional exact buffer-source scope:
auto one_source = index.window_statistics(window,
    pvd::memory::GroupBy::type_file_line,
    pvd::memory::Source{stream_id, buffer_instance_id});
```

`EventWindow{begin_event_count, end_event_count}` selects outer event indices in
`[begin,end)`. `native_frame_event_window` validates the supplied native marker
against the Session and finds the preceding native EndSection in exactly that
marker's `(stream_id, scene_id)`. Its first frame starts at zero, including
startup packets received before that first end. A different scene or stream's
end does not silently become the previous boundary.

This is a **global receipt window**. Native memory channels contain a buffer
source, pointer addresses and allocation metadata; they do not contain a scene
owner or allocation timestamps. Selecting a scene defines its frame boundaries,
not ownership of the allocations received between them. An application can
buffer events and flush them later. Their receipt window does not establish
when simulation allocated them. Source filtering is exact and independent of
the frame's scene/stream; it is never applied implicitly.

`Index::indexed_event_count()` reports covered outer-prefix history. A window
requires `begin <= end <= indexed_event_count()`. The Session constructor and
`update(session,prefix)` establish that coverage, including empty outer tails.
The channel-vector constructor can establish coverage only through its largest
packet event index plus one. Queries beyond known coverage fail explicitly
rather than returning fabricated zero activity. Earlier queries do not rewind
the parser; later `update` calls preserve dictionaries and address generations.

## Values and incomplete data

`WindowStats::as_of_end` contains cumulative observed outstanding/peak and
allocation/free totals before `end`. Its peak follows original inner-event
order, including an allocation freed in the same packet. `activity` counts only
inner events whose enclosing packet index is in the selected window:

| Field | Meaning |
| --- | --- |
| `allocated_bytes`, `allocation_count` | Observed allocation records in the window |
| `freed_bytes` | Sizes of frees matched to an observed allocation |
| `free_count`, `unknown_free_count` | All free records, and the subset with unknown size |
| `replaced_bytes`, `replacement_count` | Active addresses allocated again without an observed free |
| `churn_bytes()` | Exact decimal **allocated + known freed** bytes |
| `net_bytes()` | Exact signed decimal **allocated - known freed** bytes |

Churn here is an explicit new formula, not a claim to reproduce the old
application's unverified churn calculation. Unknown free sizes are unavailable,
not inferred as zero. Replacement closes the old observed lifetime separately
and is not counted as a free. Consequently `net_bytes()` alone is not always
the outstanding-byte change: replacement, missing baseline records and damaged
channels must be considered. The string arithmetic preserves negative values
outside `int64_t` and sums beyond `uint64_t`, without conversion through double.

`groups` combines each type/file grouping's cumulative endpoint and interval
activity. A known free is attributed to its allocation's original group. An
unknown free has no invented group, type or size; it remains in overall counts.
Independent buffers and streams retain independent dictionaries and address
generations even when addresses and buffer IDs coincide.

`has_memory_channel` distinguishes a received empty memory packet from no
memory channel observed before the endpoint. `packets_in_window` counts actual
packets, including empty or quarantined-source packets. `complete=false` and
copied diagnostics preserve relevant prefix failures; an already quarantined
source does not become complete merely because no records were decoded in a
later interval. Malformed, unsupported wire FullAllocationEvent, unknown tags
and limit failures retain only the previously decoded inner prefix. No stream
resynchronization or unknown allocation size is invented.

`Options::max_packets` bounds retained receipt metadata even for empty packets
or packets from a quarantined source (default 4,194,304). Exhausting this global
budget stops indexing explicitly with `limit_exceeded`; it never silently drops
packet metadata while reporting a complete later window.

## Viewer behavior

The Memory panel displays cumulative history alongside **Received in selected
native frame**, allocated/freed bytes and event counts, known-byte churn/net,
native-end outstanding/peak, and replacement/partial diagnostics. All values
are actual copied index results; this is not a placeholder table.

While viewing an explicit after-fetch sample, the panel chooses the latest
preceding native end for that scene. It names this boundary and explains that
later flush packets are excluded. Such packets enter a later receipt window;
they are not moved backwards to a guessed simulation step. Without a preceding
native frame, the panel says per-frame activity is unavailable. The existing
history “At current frame” toggle and type/text filters do not redefine the
native receipt interval. Exact integer byte/count fields do not pass through
floating-point formatting.

## Validation

`tests/memory_window_tests.cpp` covers LE/BE with 32/64-bit compressed address
fields; source isolation; same-packet allocate/free order and peak; normal
reuse and duplicate-address replacement; unknown frees; empty/absent channels;
malformed/full-allocation/limit quarantine; exact arithmetic; native scene and
stream boundaries; incremental coverage; late receipt and reverse queries.

With arguments `capture.pxd2 summary.csv`, the same SDK-free test reads the
existing real PhysX 3.4 `eMEMORY` producer's five validation sections and compares
every `MemoryFixture.*` group's allocation/free activity to independent allocator
callback CSV deltas, forward and backward. Those sections are custom phase
markers, not falsely advertised as native simulation frames. Initial SDK
pre-listener unknown frees remain incomplete diagnostics while the independently
known fixture allocation fields still match.

`tests/memory_window_panel_tests.cpp` runs actual ImGui controls at 1x/1.5x/2x
fonts, style and physical dimensions. It checks preceding native-end activity,
after-fetch late packets, the cumulative-history toggle, reverse seek and
nonoverflowing summaries at a logical 1060x700. It requires no SDK or OS window.
