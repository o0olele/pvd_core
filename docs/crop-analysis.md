# Analysis state at a crop boundary

A cropped recording contains a synthetic initial-state bootstrap followed by the exact selected outer-event range `[begin_event_count, end_event_count)`. The original raw channel packets in that range retain their bytes, source stream IDs, and ordering. A channel property snapshot is only its latest packet: it is not a replacement for the channel's dictionary or open state.

The core writer exposes `writing::CropOptions::channel_bootstrap` for reconstruction supplied by an optional analysis library. Core does not link to analysis, PhysX, ImGui, or Windows. Each provider identifies its original stream ID, buffer instance ID and generation, property name, semantic, and canonical payload bytes. The writer checks that every relevant live source has exactly one matching provider. It preserves the source stream's existing outer name handles when it writes the bootstrap property, so subsequent original packets keep the same interpretation. Unknown or missing reconstruction raises `CropUnsupported`; full save and a zero-start prefix remain available.

Both crop endpoints must be outside property/chunk/message-group transactions. The regular object bootstrap preserves object lifecycle generations through creation/destruction skeletons; it does not replay old simulation poses or debug primitives. Memory-allocation generations have a separate limitation described below.

## Memory bootstrap

`pvd::memory::make_bootstrap(session, begin_event_count, options)` produces bounded canonical memory bytes for each source whose buffer is live at that prefix. A source is `(outer stream_id, buffer_instance_id)` and also carries its property name and current buffer generation for writer validation.

For each retained source, the helper writes:

1. Every string handle declared before the prefix, once, in handle order. This includes currently unused handles, because later selected packets can refer to them without declaring them again.
2. Only allocations still active at the prefix, in address order, with their original address, size, type/file handles, and line.

No old frees, retired allocation history, future strings, or later deallocations are included. The source byte order is retained. Canonical headers use compression code 3; addresses are eight bytes and logical `uint32_t` size/handles/line remain four bytes, as required by the SDK's wire overload. Source32 and source64 captures use the same logical memory-event format, independently of the host ABI. Strings retain the mandatory NUL and their original handle values, including empty strings.

Sources belonging to destroyed buffers are not resurrected. Their old diagnostics do not contaminate a new, unrelated live-source baseline. Reuse of a buffer instance ID is rejected because the original memory dictionary generation is ambiguous. Multiple memory-semantic properties sharing one `(stream, buffer)` source are also rejected rather than combining their dictionaries.

The helper validates the prefix through the existing memory index. It reads lifecycle metadata directly rather than copying every object/property snapshot. It has separate limits for input memory bytes, source count, retained dictionary entries, retained active allocations, and total canonical output bytes. Existing decoder limits additionally bound historical events, allocations, string lengths and packet sizes. Every canonical source packet must fit the configured packet limit and a `uint32_t` item count. Output sizes are checked before output payloads are allocated; a failure returns no partial bootstrap.

The following adapter uses the existing core writer without adding a core-to-analysis dependency:

```cpp
auto baseline = pvd::memory::make_bootstrap(session, begin);
pvd::writing::CropOptions options;
for (const auto& source : baseline.sources) {
    pvd::writing::ChannelBootstrap channel;
    channel.stream_id = source.source.stream_id;
    channel.instance_id = source.source.buffer_id;
    channel.instance_generation = source.buffer_generation;
    channel.property_name = source.property_name;
    channel.semantic = "memory event stream";
    channel.bytes = source.bytes;
    options.channel_bootstrap.push_back(std::move(channel));
}
auto crop = pvd::writing::crop_events(session, begin, end, options);
```

If profile channels are also present, their state must be supplied by a profile provider as well. A memory provider does not satisfy a profile requirement. The core can accept further provider semantics without changing the memory implementation.

## Strict and partial baselines

Strict is the default. Any relevant live-source diagnostic rejects bootstrap construction: malformed/truncated events, unknown/full-allocation tags, missing/conflicting string mappings, duplicate allocations, and exhausted resource limits are not recoverable by guessing.

A free with no observed allocation can legitimately occur when an SDK allocation predates listener registration. An explicit `UnknownFreePolicy::allow_partial` allows **only** this diagnostic. The result then has `partial = true` and retains the original source/event/payload-offset messages. Addresses and sizes for unobserved allocations are not invented.

An adapter must propagate this state to each affected `ChannelBootstrap.partial`, include the relevant diagnostic messages in its `warnings`, and explicitly set `CropOptions.allow_partial_analysis = true`. The writer otherwise rejects partial reconstruction. It persists both a baseline warning and supplied source messages as typed wire `ErrorMessage(2)` events. `CroppedStream.warnings` also exposes them to the UI. Reopening the exported raw recording therefore preserves the warning provenance even though the reconstructed memory stream starts with internally consistent observed allocations. An internally consistent cropped index does not retroactively prove that the original recording observed every allocation.

Complete byte-for-byte history, including its original diagnostics, remains available through full save.

## Memory statistics after cropping

The synthetic live allocations define the cropped recording's baseline. Cropped cumulative allocation counters and peaks are relative to that baseline, rather than to the discarded history. For the retained source set, if `B` is original state at the crop start and `E` is state at a later selected prefix:

| Cropped statistic | Definition |
| --- | --- |
| Current bytes/count | Same observed active state as `E` |
| Total allocated bytes | `B.current_bytes + E.allocated_bytes - B.allocated_bytes` |
| Allocation count | `B.current_allocations + E.allocation_count - B.allocation_count` |
| Observed freed bytes/count | Original counter increase after `B` |
| Replaced bytes / unknown frees | Original counter increase after `B` |
| Peak bytes/count | Maximum of the synthetic initial state and subsequent selected inner-event states |

These formulas apply per retained source/group as well as to their sum. A retired source whose buffer is absent at the crop start is excluded from the continued source set; its last observed memory state is not presented as a new live recorder.

Memory wire events do not contain allocation generation numbers. Retained active allocations begin at generation zero in the cropped index; subsequent reuse is counted from that cropped history. Recovering the original allocation-generation ordinal would require replaying discarded alloc/free history, which would violate the canonical state-only bootstrap. Original object-instance generations are preserved by the separate core lifecycle skeleton. Memory allocation times are not invented: the wire supplies ordering, not individual allocation timestamps.

## Verification

`tests/memory_bootstrap_tests.cpp` uses independent exact wire fixtures and the real core writer. It covers little/big endian, source32/source64, source-isolated equal addresses, prior address reuse, retained unused/UTF-8/empty dictionary strings, omitted retired allocations, future-string isolation, selected raw packet continuation, explicit generation rebasing, and exact baseline/peak accounting. It writes a cropped Session back to raw bytes and reparses it through the independent protocol decoder. It checks input/output/source/dictionary/active/decode budgets, corrupt or undefined input, strict versus explicitly partial unknown frees, retired-source diagnostics, and buffer-ID reuse.

With the actual PhysX 3.4 `eMEMORY` fixture, four middle-of-recording starts (validation phases 0, 1, 2, and 3) were each cropped through phase 4. Canonical bootstraps were approximately 27 KiB rather than all historical allocation packets. The cropped active-address set, size/type/file/line fields, current counters, and rebased cumulative counters matched the original selected endpoint. The partial source warnings remained typed `ErrorMessage` events. This uses the same fixture whose allocation fields and phase totals are independently verified against the real allocator callback by `tests/physx34_memory.cpp`.

SDK-internal allocation addresses and counts vary by run. Validation compares the original and cropped recording from the same generated run; it does not hard-code heap addresses.
