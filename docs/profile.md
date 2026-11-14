# CPU profile analysis

`pvd::analysis` contains the SDK-independent `pvd::profile::Index` in
`pvd/profile.hpp`. It consumes the `profile event stream` buffered channel used
by PhysX 3.4. It does not link PhysX, open a network connection, or depend on ImGui.

```cpp
#include <pvd/profile.hpp>

auto session = pvd::Session::load_file("capture.pxd2");
pvd::profile::Index index(session);
pvd::profile::Query query;
query.thread_id = 101;
query.text = "simulate"; // case-sensitive substring of zone or event name
for (auto row : index.query(query)) {
    const auto& span = index.intervals()[row];
    // Native ticks are always retained. Nanoseconds are optional.
    auto nanoseconds = span.duration_nanoseconds;
}
index.write_csv(output_stream, query);
```

For a live Session, construct `Index index(session.header())`, then call
`index.update(session)` after each successful append. The default constructor
also adopts the first bound Session's clock. Each update visits only newly
appended outer events and channel packets. `update(session, event_count)` can
advance through an immutable prefix, but cannot rewind or switch Sessions.
Call `finish()` when reception ends. The constructor taking a Session performs
this automatically if that Session is finished or failed. An index is not
internally synchronized; callers must serialize access to its Session and index.
Returned references remain owned by the index and must not be retained across
updates. Low-level users can register zone/event names and `append_packet()`
without constructing a Session. Other channel semantics are ignored.

## Encoding and time

The local PhysX 3.4 SDK's `PxShared/src/pvd/src/PxProfileEvents.h` defines a
four-byte header: tag `u8`, compression flags `u8`, and event ID `u16`. All
multi-byte fields follow the capture's byte order. Width codes 0, 1, 2, 3 mean
1, 2, 4, 8 bytes. Tags 1/2 carry a full thread/context/priority/CPU description;
tags 3/4 reuse the last full context. The low two flag bits select timestamp
width, and the next two select context-ID width for full events. Timestamp
widths below eight bytes encode a delta; eight bytes encode an absolute value.
A four-byte delta is still a delta, not an absolute truncated timestamp.

`PxProfileDataBuffer.h` resets the cached context and last timestamp at every
flush. `PxProfileEventParser.h` constructs fresh decompression state for each
buffer. Consequently compression state is **packet-local**. A relative event
before a full context in a new packet is reported, rather than reading an
uninitialized SDK parser context. Full contexts later in a packet may change
thread or context; timestamp deltas continue from the previous CPU record.
Counter values do not change this inherited context or timestamp.

CPU timestamps come from `Time::getCurrentCounterValue()` in
`PxProfileEventBuffer.h`. The legacy field name `mTensOfNanoSeconds` does not
describe the actual encoded units. The stream initialization in
`PxPvdCommStreamEvents.h` already multiplies the foundation's tens-of-nanoseconds
numerator by ten. Thus the capture header supplies **nanoseconds per tick**:

```text
duration_ns = duration_ticks * timestamp_numerator / timestamp_denominator
```

The analysis uses floating-point nanoseconds to retain fractions and avoids
integer product overflow. A zero numerator or denominator produces an
`invalid_clock` diagnostic and empty nanosecond values. Native ticks remain
available. The real Windows SDK fixture checks its header against
`QueryPerformanceFrequency()` independently: numerator 1,000,000,000,
denominator QPC frequency. It does not apply another factor of ten.

## Names, pairing, and queries

`AddProfileZone` and `AddProfileZoneEvent` provide names, IDs, and the advertised
compile-time enabled flag. Missing metadata produces diagnostics and explicit
fallback names, rather than dropping timing records. Zone identity includes a
generation so a destroyed/recreated announced zone cannot pair with an earlier
scope using the same pointer ID. Generation zero is the first observed profile
announcement; unannounced lifetimes before that announcement are not inferred.

Start/stop pairing survives packet boundaries and is LIFO within
`(zone generation, thread ID, context ID, event ID)`. Recursive calls of one event
ID pair correctly. Different threads and contexts never pair accidentally.
The SDK's cross-thread sentinel is retained as the thread ID it actually sends.
An absolute timestamp may move backwards between unrelated threads or explicit
`atEvent` calls; only a stop earlier than its own matching start is rejected.
Equal timestamps produce a legitimate zero-duration interval. `depth` allocates
non-overlapping display lanes within each zone/thread/context; normal nesting
uses increasing depths, and crossing event IDs reuse vacant lanes.

Queries filter by zone ID/generation, event ID, thread, context, a case-sensitive
zone/event-name substring, outer source-event range, and native-tick range.
An event-count range returns intervals whose **stop** lies in that range. A
start may precede its beginning. This makes current-frame analysis useful for
scopes crossing the frame boundary without pretending packet receipt time is
the CPU timestamp. Time ranges test overlap with `[begin_ticks, end_ticks)`;
an interval ending exactly at the lower bound is excluded. A frame's received
event prefix is independent of counter time. The producer may buffer scopes
for several simulation frames before sending them.

`aggregate(query, group_by_thread)` and `write_csv(stream, query)` use exactly
the same filter as `query()`. Totals, means, minima and maxima are inclusive;
child time is not subtracted, so summed nested scopes are not CPU utilization.
Aggregation always separates zone generations and event IDs. CSV escapes names
using RFC 4180 quoting and includes source start/stop event indices, raw ticks,
optional nanoseconds, thread/context, CPU, priority, and display depth.

## Explicit partial-data behavior

Tag 5 is a counter value, not a timestamped CPU interval. The index retains its
raw signed 64-bit value and emits an informational `value_without_timestamp`
message. PhysX 3.4's `EventValue::getValue()` casts to `int16_t` despite returning
`int64_t`; the API preserves that separate `sdk_legacy_value` view and warns
when it differs. The real SDK oracle confirms 70,000 is exposed as 4,464 by
the SDK callback. The independent analysis retains both values.

Tag 6 is the obsolete CUDA-buffer placeholder that the PhysX 3.4 parser merely
skips. The analysis validates both length fields, bounds the payload, retains
version/byte-count/source metadata, and emits `unsupported_gpu`. It does not
fabricate CPU intervals or claim GPU timing support. No profile channel,
only counter values, unsupported GPU timing, and genuinely zero CPU duration
are distinguishable in the event list, diagnostics, and statistics.

Unknown tags cannot be skipped safely because the format gives them no length.
Malformed/truncated packets preserve successfully decoded prior records and
completed intervals, then abandon open scopes in that zone. Later stops become
unmatched instead of pairing through missing data. Zone destruction and
`finish()` also report outstanding starts; no duration is synthesized. Empty
flush packets are valid. Reserved flags, array type/count mismatches, timestamp
overflow, reversed intervals, and malformed CUDA lengths are diagnosed.

`Options` bounds each packet's bytes, total decoded events, intervals, open
scopes, zones, event names, name length, and retained diagnostics. Exhausting a
data budget sets `statistics().saturated`, stops indexing further data, and
reports the resource limit. Diagnostic overflow is counted separately. Warning
or error diagnostics set `complete=false`; informational untimestamped-value
messages alone do not. Consumers should display these flags beside timing
results. This module does not promise transactional recovery after allocation
failure; discard the index if a C++ allocation exception escapes.

## Validation

`tests/profile_tests.cpp` covers little/big endian encodings, every compressed
field width, absolute/relative timestamps, packet-local context, cross-packet
pairing, recursion and crossing scopes, legitimate zero duration, thread/context
isolation, live Session prefixes and zone reuse, source/time/search filters,
aggregation/CSV, signed values, GPU payloads, every truncation of a small
start/stop buffer, corrupt prefixes, overflow, and configured budgets.

With `PVD_BUILD_PHYSX34_VALIDATION`,
`examples/physx34_profile_capture.cpp` creates a real `ePROFILE` file transport
and real SDK `PxProfileZone` buffers. It sends two named zones, explicit thread
and context IDs, compressed/absolute times, recursive scopes, an open scope
across flushes, and a counter demonstrating the SDK truncation. Its independent
expected CSV comes from the SDK's inline `parseEventData` implementation,
before `pvd_core` reads the capture. The producer links only the supplied Debug
SDK DLL import libraries, uses `/MDd`, and never builds or modifies the SDK.
Run `pvd_profile_tests capture.pxd2 expected.csv` for record-by-record and
independently paired duration/calibration comparison. The generated fixture
currently contains 15 profile records and seven completed CPU intervals.

## Cropping a buffered profile continuation

`pvd/profile_bootstrap.hpp` adds the independent analysis-layer
`profile::make_bootstrap(session, prefix, options)`. It indexes only that prefix
without calling `finish()`, reconstructs outstanding recursive start stacks,
and returns canonical bytes for each still-live profile buffer. Map each
`SourceBootstrap` into `writing::ChannelBootstrap` with its original
`stream_id`, `zone.id`, `buffer_generation`, `property_name`, the
`profile event stream` semantic, and `bytes`. Pass these entries in
`writing::CropOptions` when preparing the crop. Core remains independent of the
analysis library.

Only outstanding starts and the latest raw counter per event/thread/context
are retained. Full context fields and absolute 64-bit start timestamps make
the baseline independent of packet compression history. Recursive starts
remain in their original arrival order; completed old intervals and old stops
are excluded. Subsequent original packets reset their own compression state
and pair with those starts normally. Canonical counter records remain
untimestamped. `records` maps each canonical payload offset to its original
`Source`, because a synthetic start's source event index/byte offset necessarily
changes. Stream IDs, zone generations, CPU/priority, context and exact native
ticks are preserved. Wire/header calibration and SDK legacy-counter diagnostics
remain visible after reopening the crop.

Limits bound input/output bytes, source count, outstanding scopes and retained
counters, in addition to the profile index's limits. Corrupt/truncated packets,
unsupported GPU data, unresolved names, unmatched stops, invalid intervals and
lost/saturated diagnostic history reject reconstruction for an affected live
zone. Earlier diagnostics belonging only to a destroyed zone generation do not
invalidate a clean new generation. Clock-unavailable and raw-versus-SDK counter
limitations remain explicit supported diagnostics. One live zone using several
properties or source streams is conservatively rejected: grouping one packet
per source could reorder recursive starts across those sources. The native
SDK fixture gives each zone its own dedicated stream.

The bootstrap tests exercise LE/BE and 32/64-bit pointer headers, high-bit
64-bit ticks/context, multiple threads, nested recursion, latest counters,
zone-pointer reuse with generation preservation, corruption/GPU/limit rejection,
and writer/crop/reload continuation. Four real SDK packet-boundary cuts include
the open start on one side of a flush and its stop on the other; their resulting
intervals match the original ticks, calibrated duration and source identity.
