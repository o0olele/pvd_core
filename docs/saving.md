# Saving and cropping captures

`pvd/writer.hpp` is part of `pvd::core` and has no analysis, renderer, SDK, or UI
dependency. It writes the PhysX 3.4 SDK **raw communication stream**, suitable
for this project's loader. It does not produce the original PVD application's
indexed saved container. Use a distinct `.pxd2` filename.

```cpp
#include <pvd/writer.hpp>

pvd::writing::WriteOptions options;
options.cancelled = [&] { return stop_requested; };
auto result = pvd::writing::save_session(utf8_filename, session, options);
if (result.state == pvd::writing::WriteState::cancelled) {
    // No destination was published.
}
```

The default refuses existing destinations. Only pass `overwrite=true` following
an explicit user choice. A new private sibling file is exclusively created,
written, flushed, and closed before publication. Publication atomically refuses
a destination that appeared concurrently, or replaces it when overwrite was
requested. Exceptions and cancellation remove the private file and leave an
existing destination untouched. Windows uses wide UTF-8 path conversion and
`MoveFileExW`; POSIX uses exclusive `open`, `link` for no-overwrite publication,
and `rename` for replacement. Embedded-NUL paths are rejected. Directory-entry
durability after power loss is not guaranteed. A process crash can leave a
clearly named `.pvd-tmp-*` sibling, never an intentionally published partial
destination. Existing readers can prevent replacement on Windows; that failure
preserves the old file.

All 28 event alternatives and the 28-byte stream header are encoded in their
original little/big byte order. IDs, counters, timestamps, string handles,
message offsets, and reference IDs stay integer values; no JSON or floating
point intermediary is involved. Property, chunk, message, resource and embedded
channel payload bytes remain byte-exact. Strings retain their UTF-8 bytes and
reject embedded NUL. Logical booleans are normalized to zero/one, and empty
wire strings use the SDK's zero-length representation. This is event/state
preservation, not a byte-identical copy of the original file.

Events retain source order. Only adjacent events with the same stream ID and
outer timestamp can share a group. `target_group_bytes`, `max_group_bytes`, and
`max_events_per_group` control repacking; an event larger than the target may
occupy its own group, up to the hard limit. An event cannot be split implicitly
because opaque message/channel formats may depend on its boundaries. A hard
budget violation fails explicitly. `max_output_bytes` bounds total output.
Original byte offsets are replaced by offsets in the newly packed file. Inner
section timestamps and embedded-channel timing bytes are preserved.

`encode_header` and `encode_event` expose low-level encoding.
`write_raw(ostream, header, events)` encodes caller-supplied events without
semantic Session validation. The header's order must already match opaque
payload encoding; choosing a different header does not transcode those bytes.
The ostream APIs throw on write failure but cannot roll back caller-owned
streams. Cancellation returns `WriteState::cancelled` and accurate bytes/groups
already written. Such output is partial and must not be presented as a completed
save. File APIs handle publication/cleanup for this reason.

## Live and failed Session prefixes

`safe_prefix(session, requested_count)` scans accepted source events, tracking
property chunks and message groups independently on every stream. It returns
the latest global prefix with no open property transaction. Ordinary sections
can remain open, matching `Session::finish()` semantics. A boundary can lie
inside an old wire group because the writer repacks complete new groups.
`write_session` and `save_session` export that prefix and report
`requested_event_count` and `discarded_tail_events`. They can operate on a live
or failed Session, including one whose state recovery is unavailable, because
they require only its accepted source events and header. They never serialize
unaccepted events or fabricate the end of an unfinished transaction.

The Session must be externally synchronized while saving. Copying a live source
prefix under its owner lock and exporting that immutable copy avoids holding
the lock during disk I/O. Cancellation is polled between encoding events and
writing groups, including after the final blocking write. It is cooperative;
one event/group or filesystem operation can take time to finish.

## Range crop and initial state

`crop_events(session, begin, end)` selects the half-open **source event-count**
range `[begin,end)`. Both endpoints must be transaction boundaries; they are
not silently rounded. A zero-start range is an ordinary prefix, including
embedded analysis data unchanged. `crop_frames(session, first, last)` accepts
inclusive global native-frame indices. It starts with the first frame's
`BeginSection` and includes the last `EndSection`. Interleaved scenes retain
their original event ordering. Other sections already open at the crop start
are bootstrapped so their later ends remain valid.

For a nonzero start, `CroppedStream.bootstrap_event_count` separates a synthetic
initial state from the selected original events. The bootstrap contains:

- Original metadata and stream-local string-handle declarations needed by
  selected typed and opaque payloads.
- Creation/destruction skeletons and profile-zone announcements in their
  original relative order, preserving object/zone reuse generations and
  creating streams. Old poses and property changes are excluded.
- Live-instance property state, references, flags and optional colors.
  Large property state is emitted in bounded chunks.
- Cameras, accumulated explicit origin shifts and open ordinary sections.

Snapshot property encodings are preserved, including exact 64-bit values and
pointer-string fixups. Synthetic stream IDs avoid every original source stream.
StringHandle property values get isolated synthetic contexts with their
original numeric handle and resolved text, avoiding collisions with future
source declarations. Old simulation frames, motion, debug packet histories,
and closed timing scopes are not replayed. The retained lifecycle skeleton is
needed because the wire format has no explicit generation field. A crop may
still have substantial metadata/resource/bootstrap overhead. Its initial
snapshot and range-end snapshot are compared against the source in tests.

Source byte offsets and event indices change. Use `source_begin_event_count`,
`source_end_event_count` and `bootstrap_event_count` for navigation between the
two event spaces. Frame ordinals restart, while scene IDs and timestamps stay
unchanged. This API requires available Session state. Unsupported bootstrap
representations, such as more than `uint32` property items, raise
`CropUnsupported` instead of truncating the state.

## Stateful analysis channels

Copying only the most recent profile/memory packet would corrupt later packet
interpretation. A nonzero crop with prior data from a still-live analysis buffer
requires complete `CropOptions.channel_bootstrap` entries. Each carries the
original stream ID, buffer ID/generation, property name, semantic, and canonical
bytes. Core validates the required source set and uses existing wire handles
on those original streams. It replaces stale snapshot packet properties with
the supplied analysis baseline. Core does not interpret that inner protocol;
the analysis layer is responsible for its reconstruction and validation.

Memory bootstrap must contain the prefix dictionary and live allocations;
profile bootstrap must contain outstanding CPU scopes and required counter
state. Without a provider, `CropUnsupported` reports the problem. Fully closed
data from an already destroyed buffer does not require recreation. The
independent analysis helpers document their own support and conservative
rejection rules. Rebased memory totals mean allocations active at the boundary
form the initial baseline; earlier allocation/free totals are outside the crop.

Partial analysis reconstruction is refused by default. Explicitly opted-in
`allow_partial_analysis` entries carry `partial=true` and their source warnings.
The crop retains them in `CroppedStream.warnings` and persists warning
`ErrorMessage` events (`code=2`, file `pvd_core/crop`) so reopening the file does
not hide earlier incomplete-baseline evidence. UI consumers must show these
warnings next to results; a cleanly parsed inner baseline does not erase the
provenance warning.

## Verification

`pvd_writer_tests` covers all 28 events, LE/BE, exact high-bit `uint64` fields,
opaque bytes, strings and handles, groups/budgets, messages/chunks/references,
live safe prefixes, synthetic initial-state/end-state equality, ID reuse,
large properties, cameras/origin/flags, explicit analysis providers, partial
warning persistence, UTF-8 filenames, cancellation, exclusive creation,
failed-overwrite preservation, cleanup and output-stream errors.

Passing SDK capture filenames runs full real-capture roundtrips, preserving
events plus state/frame/channel indices. Supported motion/render samples also
run middle-frame crops and compare initial/final state. Current validation uses
real PhysX 3.4 physical, custom, render, profile and memory fixtures. Analyses
are preserved in full saves; their separate bootstrap tests validate cropped
continuations.
