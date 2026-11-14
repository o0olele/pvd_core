# PhysX 3.4 memory and error analysis

`pvd::analysis` is an optional C++17 library that depends only on `pvd::core`. The memory and error headers do not depend on PhysX, ImGui, a GPU API, or Windows. The SDK is needed only to build the independent validation producer.

## Source evidence and wire format

The implementation follows the local PhysX 3.4 source rather than the layout of host C++ structs:

- `PxShared/src/pvd/src/PxPvdImpl.cpp`, `initializeModelTypes`: `_debugger_::PxProfileMemoryEventBuffer.events` is a `PvdU8` array with semantic `memory event stream`.
- `PxShared/src/pvd/src/PxPvdMemClient.cpp`, `onAllocation`, `onDeallocation`, `handleBufferFlush`: the client feeds the SDK memory-event buffer and sends its bytes through that property.
- `PhysX_3.4/Source/PhysX/src/NpPhysics.cpp`: `NpPhysics` registers `PvdImpl` as the Foundation allocation listener.
- `PxShared/src/pvd/src/PxProfileMemoryEvents.h`: header masks and event field order.
- `PxShared/src/pvd/src/PxProfileEventSerialization.h`: compressed scalar serialization and NUL-terminated string encoding.
- `PxShared/src/pvd/src/PxProfileMemoryEventBuffer.h`: string handles persist between flushes; a whole event is written before a flush can occur.
- `PxShared/src/pvd/src/PxProfileMemoryEventParser.h`: accepted serialized tags and rejection of the in-process FullAllocation event.

Every inner memory event starts with a source-endian `uint16_t` header:

| Bits | Meaning |
| --- | --- |
| 0–3 | Tag: 1 string, 2 allocation, 3 deallocation |
| 4–5 | Address compression |
| 6–7 | Type-handle compression |
| 8–9 | File-handle compression |
| 10–11 | Allocation-size compression |
| 12–13 | Source-line compression |
| 14–15 | Reserved; unsupported if set |

Compression codes 0, 1, 2, and 3 select 1, 2, 4, and 8 bytes for the logical `uint64_t` address. For logical `uint32_t` size, handles, and line, code 3 still selects **four bytes**, as required by the SDK's `uint32_t` overload. Address compression is independent of source pointer width; a 32-bit capture is not decoded using host pointers. The outer stream byte order controls every multibyte inner scalar.

The bodies are:

- String: `u32 byte_length`, bytes including exactly the final NUL, `u32 handle`. The header compression fields do not affect this body.
- Allocation: compressed size, type handle, file handle, source line, then address.
- Deallocation: compressed address.

Allocation sizes are logical `u32`: the SDK casts the original `size_t` before serialization. A larger original allocation cannot be recovered from these bytes. Address zero allocations are suppressed by the SDK and rejected as malformed if encountered.

`FullAllocationEvent` (tag 4) contains native `size_t` and `const char*` fields for the SDK's **in-process** outstanding-allocation recorder. It has no wire `streamify` method. The SDK parser asserts that it never appears on the wire. The recorder replays it through `onAllocation`, producing string-table and ordinary allocation events. This implementation resolves those ordinary events into full allocation records. A literal wire tag 4 produces `unsupported_full_allocation`; it is never decoded by guessing pointer layouts.

## Index and seek semantics

```cpp
pvd::memory::Index memory;
pvd::errors::Index errors(std::vector<pvd::Event>{});
// After appending complete events to this same Session:
memory.update(session);
errors.update(session);
auto usage = memory.summary_at_event(frame.event_count); // no active-row copy
auto rows = memory.history_at_event(frame.event_count, {}, 0, 1000);
```

`update(session, prefix)` reads only newly exposed channel packets before that outer event-count prefix. Its parser dictionaries and address generations persist. Repeated calls do not duplicate events. Queries at older prefixes do not rewind the parser. Create a new index for a different Session; combining two Session objects is rejected. Session append and index update/query must be serialized by the caller, just like the core Session API. The viewer performs analysis on its UI thread after receiving committed session events.

A source is `(outer stream_id, memory_buffer_instance_id)`. Sources have independent string tables, active-address maps, and address generations. The index preserves observed allocation/deallocation history, not just the latest property value. The same address can have multiple generations after reuse. If an active address is allocated again without a recorded free, the previous allocation ends with `replaced_without_free`, a diagnostic is emitted, and its bytes are counted separately from observed frees.

A packet at outer event index `i` becomes visible at event-count prefix `i + 1`. All its inner events share that prefix. `allocated_memory_index` and `ended_memory_index` preserve their inner ordering. The memory protocol provides **no allocation timestamps**. Outer packet clocks are not presented as invented per-allocation times. `history_at_event` hides later end/free information when seeking backwards. Raw `allocations()` is full indexed history and can contain future ends; frame-dependent UI uses the prefix query.

`Snapshot` includes active allocations, cumulative and peak bytes/counts, groups, and diagnostics visible at the requested prefix. Groups can use type, file, file/line, or type/file/line. `summary_at_event` returns exactly the same totals, groups, and diagnostics without copying the active allocation vector; `snapshot_at_event` preserves its original full-snapshot behavior. Peaks follow observed inner-event order, including allocations freed within a single packet. A known free updates the original allocation's group. An unknown free increments a separate counter; its size and type are not invented. Names are optional if a packet references missing string handles.

Malformed, unknown-tag, conflicting-string, or limit-exceeding input keeps the already decoded inner-event prefix and quarantines that source. It cannot be resynchronized without independent event lengths. Other sources continue. Subsequent snapshots include the failure diagnostic and `complete = false`; remaining active rows are the **last observed state**, not proof that every allocation remains live. Unknown frees and missing metadata also make `complete` false. An unknown free can legitimately occur when recording begins after an allocation, including SDK setup allocations made before the listener is registered.

Unexpected C++ failures during incremental memory update invalidate derived queries until the index is rebuilt; partial bookkeeping is not exposed as a successful snapshot. The original Session bytes/events remain available for a retry. Ordinary parse failures are represented as bounded diagnostics rather than thrown past a UI frame.

Defaults bound each packet to 64 MiB, each string to 1 MiB including its NUL, each source dictionary to 1,048,576 entries, total inner events to 4,194,304, and allocation history to 2,097,152 records. Options can reduce these limits. All reads validate available bytes before accessing them. The protocol/core layer separately limits outer event counts and blob sizes. Query results are copied; history supports offset/limit pagination. Index history is retained for seeking and is intentionally not an unbounded live ring buffer.

## Errors and CSV

`pvd::errors::Index` indexes typed outer `ErrorMessage` events with their original code, message, file, line, stream ID, timestamp, byte offset, and event index. It supports incremental `update`, half-open event-index ranges, exact code/stream filters, substring message/file searches, pagination, and CSV. The names correspond to `PxShared/include/foundation/PxErrors.h`: 0, 1, 2, 4, 8, 16, 32, 64, and 128. Unknown codes retain their numeric value.

Both CSV writers quote strings and double embedded quotes, preserve UTF-8 and embedded newlines, and write CRLF records. Memory CSV includes source IDs, hex address, generation, size, resolved names, line, start/end prefixes, end reason, and explicit resolution flags. A missing name is not silently substituted for an empty resolved string. Prefix exports hide future frees. `write_groups_csv(out, prefix, group_by)` exports the full observed grouping at that prefix, independently of allocation-history search/active filters. It includes every group statistic, the prefix and grouping key, resolution flags, and completeness. Integer counters are emitted directly in decimal, preserving all `uint64_t` bits rather than passing through floating-point display units. Text matching is ASCII case-insensitive and preserves non-ASCII bytes; it does not claim Unicode case folding.

## Validation

`tests/memory_tests.cpp` independently writes exact event bytes and checks both byte orders, source32/source64, all compression codes, cross-packet dictionaries, source isolation, reuse/replacement, prefix history, same-packet alloc/free, group/cumulative/peak accounting, malformed tails at every allocation byte, string corruption, unsupported tags, reserved bits, and parser limits. It compares incremental analysis of a growing Session with full decoding. `tests/error_tests.cpp` checks original metadata, known/unknown codes, range/filter/page behavior, CSV escaping, and incremental no-duplicate behavior.

`examples/physx34_memory_capture.cpp` links only the real SDK. It enables `eDEBUG | eMEMORY`, creates PhysX with outstanding-allocation tracking enabled, and calls the real Foundation broadcasting allocator after `NpPhysics` installs the PVD listener. Its raw allocator callback records expected fields independently. Two aligned backing blocks guarantee two real address reuses. Five explicit flush/section boundaries cover four allocations and their frees; SDK `PvdDataStream::sendErrorMessage` emits five independently expected errors.

`tests/physx34_memory.cpp` links only the independent core/analysis libraries and verifies those five boundaries, every allocation field/generation/lifecycle, grouped counters, peak usage, forward/reverse seek, incremental update, and typed errors against the callback CSVs.

The local standalone run used `analysis/memory-sdk-test/CMakeLists.txt` and produced:

- `build/memory-sdk-standalone/fixtures/real_memory.pxd2`: 128,769 bytes, 1,559 outer events, 1,924 inner memory events, 1,411 observed allocations.
- `real_memory_expected.csv` and `real_memory_summary.csv`: all five phase prefixes matched; fixture peak 78,192 bytes and two address reuses.
- Fourteen source diagnostics were all unknown frees for SDK allocations outside the observed allocation prefix; no malformed/unsupported memory payloads occurred.

Allocation addresses and SDK-internal counts can vary across runs. The integration test compares callback-produced expectations from the same run rather than hard-coding addresses.
