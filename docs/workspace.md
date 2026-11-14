# Workspace and saved views

The viewer workspace is a UI-independent owner of multiple `Document` objects. It does not own a TCP receiver, an export worker, an ImGui window, or an application settings location. No file is read or written automatically. The host chooses when to use an explicit `.pvdws` path; smoke tests can leave persistence disabled.

## Stable slots and owners

`Workspace::add` and `create_empty` return a nonzero 64-bit `SlotId`. The slot and its Document are heap allocated. Adding tabs or switching the active ID preserves their addresses, frame/selection state, camera, and view options. `active()` can be null when all tabs are closed. An explicit unlocked `replace` replaces that slot's Document; `close` destroys that slot. `replace` moves the supplied `unique_ptr` only on success.

Each slot has its own `OrbitCamera` and `camera_ready` flag. The host can fit an initial camera only when `camera_ready()` is false, then call `set_camera_ready()`. Restored cameras include arbitrary `reference_up`, display handedness, target, yaw, pitch, distance, field of view, and near/far planes. They are validated with the same camera math used by the viewport. Category visibility, sleeping/trigger None/All/Selected/Unselected scopes, color mode, and uniform color are persisted through `Document::ViewOptions`. Transient hide/isolate rules and selection are not serialized; selection remains intact while switching between live in-memory tabs.

`WorkspaceSlot::preferences()` owns a UI-free `ViewPreferences` value: all render-layer switches, antialiasing, RGBA background, point size, contact-normal length, particle/rigid velocity scales, inspector numeric format/precision/array page size, and debug-text visibility. It includes bounds and linear/angular rigid-velocity overlays, scene-query paths/normals/volumes, native deformables, joints and debug primitives. `StoredView` and the capture's verified `last_view` archive carry that value alongside camera and category options. These values do not alter the captured Session.

`set_default_preferences(value)` validates before replacing global defaults. Existing slots retain their preferences; new empty/source slots inherit the defaults unless an identity-verified saved view applies. Matching reloads and archived reopenings restore the saved per-slot value. Changed or unverifiable capture content falls back to the current global defaults, without applying the previous file's preferences. Restore placeholders display their pending preferences, and the actual source identity is checked again before retaining them after asynchronous load. A stopped recording's `bind_source` archives its current preferences without replacing its Document or adopting an unrelated path archive's view.

Version 5 also keeps `default_view_options()` / `set_default_view_options(value)`
for the complete Document display policy: categories, scopes, color mode,
geometry and joint sizes, planes, and the single `ColorPalette` value. New
file/empty/live slots inherit it; a verified source's stored view takes priority.
Updating defaults leaves current slots unchanged. Restored global settings and
per-source values are validated independently. The host's **Use current as
defaults** and reset actions copy/reset both the `ViewPreferences` and
`ViewOptions` values. There is no duplicate palette in RenderOptions.

Mutable slot preferences are host-owned, like the camera. `valid_view_preferences` is available without ImGui, Win32 or a D3D device. Persistence/restoration and source binding validate them: AA and numeric-format enums must be known, background channels finite in `[0,1]`, point size in `[1,64]`, contact-normal length finite/nonnegative, and particle/rigid velocity scales finite in `[0,1000000]`. Inspector precision is 1–17 and array pages are 32, 128, 512 or 2048. Invalid defaults, restored settings or file payloads leave the existing workspace unchanged. Rejected identity or preference validation happens before reserving a new capture archive.

Locks are exclusive and have `loading`, `live`, or `exporting` owners. They prevent close, replacement, and whole-workspace restore. Only the matching reason can unlock a slot. Activation of another slot remains allowed. These locks express ownership; they are not an internal mutex around Document methods. Workspace commands run on one owner/UI thread.

Host integration follows this sequence:

1. A load future records its target SlotId. It prepares a separate Document and may compute `workspace_source_identity(*document.session())` on the worker. When ready, the host unlocks the target's loading lock and calls `replace(target, incoming, path, warnings, identity)`. It never substitutes the currently active slot for the recorded target.
2. A receiver starts in a new empty slot with no `source_path`, then takes that slot's live lock. It always appends through `find(bound_id)->document()`, even if the user activates another tab. Starting live reception directly on an old file-bound Document would retain an incorrect source association and is not supported.
3. An export worker takes the original slot's export lock. The host keeps the Session immutable until the worker has stopped, then unlocks that same SlotId. Closing the popup or changing active tabs does not release the borrow.
4. After a completed recorded live session has stopped, `bind_source(id, recording_path, identity)` associates the file without moving or replacing the Document or Session. It requires an unlocked, loaded, non-live slot. Live bookmarks become associated with the final recording identity. Pure TCP sessions with no recording path remain transient.

The content-only `draw_workspace_tabs` and `draw_workspace_panel` functions use host callbacks for open/activate/close. Open callbacks receive the target ID and either a recent/placeholder path or an empty path meaning “show a chooser”. They perform no blocking capture I/O themselves. Hosts should release cached Document references before applying a close callback, or defer destructive tab commands until the end of that frame.

## Capture identity and bookmarks

Paths alone do not identify capture content. A file can be overwritten while retaining the same name and even the same number of events. Source identity therefore contains an algorithm version, exact event count, and a stable canonical FNV-1a 64 fingerprint. It hashes the encoded stream header and every event's stream ID, outer timestamp and canonical typed payload. Repacked wire groups and original byte offsets do not affect it. Raw property/channel bytes do. It is a change detector, not a cryptographic authenticity check.

Identity is cached per loaded slot, in saved slot views, in each bookmark, and in the capture's latest-view archive. A host-supplied precomputed identity must come from that same immutable loaded Session; its event count is checked. Do not pass an identity read from a sidecar as a substitute for hashing newly loaded content.

When the content at a path changes, the old view is not silently restored. Warnings are returned by replacement and exposed by the slot. Old bookmarks remain stored with their original identity and are shown as unverified for the new content. Jumping to an unverified bookmark returns false without changing the view. Bookmarks for the new content may coexist. An already open tab holding the old Session can still use bookmarks whose identity matches it.

A bookmark stores its ID, UTF-8 name, scene ID, native/after-fetch sample mode, zero-based frame index, and optional camera. It does not capture or restore render/inspector preferences. UI frame numbers are displayed starting at 1. A missing scene, absent after-fetch stream, or unavailable frame is not rounded to an unrelated location. Bookmarks are shared by tabs using the same source-path archive; each bookmark is verified against each tab's actual Session independently. Closing a tab preserves file-associated bookmarks and the latest matching view for a later reopen. Path aliases are not automatically merged.

An unsaved/live slot can have temporary bookmarks. It is excluded from disk workspace settings until a real source path is supplied. `bind_source` transfers matching bookmarks to the completed recording's archive, retains older unmatched bookmarks as unverified, and adds only that actual file path to recent files. `Document::source_label()` values such as TCP addresses are never inferred to be filenames.

Recent paths are ordered most-recent-first, deduplicated by their exact UTF-8 string, and bounded. The host should supply absolute native file paths for persistence across working-directory changes. Relative paths are stored as given. Capture files themselves are not modified by workspace operations.

## Named clip archives

Version 4 retains `clips::State` values in each source-path archive. These are
the independent core's named native-frame ranges, including exact scene
generations, original frame ordinals, stable clip IDs, selected IDs, next-ID
counters and each range's own sealed-prefix identity. See [clip ranges](clip-ranges.md).
The workspace does not replace a prefix identity with a whole-file identity:
a live source may append while an earlier sealed range remains valid.

`WorkspaceSlot::clip_states()` and `clip_catalogs()` return cached, verified
values for that slot's actual Session. The map contains ready-to-copy core
`Catalog` values keyed by scene ID/generation. `pending_clip_states()` returns
the metadata that could not yet be verified, belongs to other source contents,
or could not be combined within the configured catalog budget. A placeholder
has pending metadata and no active catalogs. Getters and panel drawing never
rehash source history. `clip_revision()` changes on explicit commands and
source classification so the host can refresh its panel cache only when needed.

`set_clip_states(slot, states)` validates loaded inputs and replaces that
Session's active catalogs. It preserves foreign/pending values. A placeholder
retains structurally valid inputs as pending, without pretending it has loaded
the source. `merge_clip_states` imports both sets: identical values are
deduplicated; incompatible same-ID clip values receive fresh IDs rather than
losing either named range. Compatible catalogs are combined by exact scene
lifetime, retaining each range's original seal. If combining would exceed the
per-catalog budget or exhaust IDs, the conflicting catalog remains pending with
a warning. A source mismatch is likewise retained, not treated as an empty list.

Ordinary setter edits replace only the slot's previously published active
records. If live append has meanwhile received a pending catalog's seal, the
setter still retains that catalog as pending until an explicit verification
or classification command. Repeated edits cannot silently consume unseen
ranges. Shared-path tabs keep independent pending masks; the prepared cache
key includes that mask as well as the verified source identity.

`verify_clip_states` explicitly retries pending values, for example after live
append has received their sealed prefix. Merely appending or drawing does not
silently promote them. Ordinary live locks permit metadata commands; loading
and export locks reject them. `discard_pending_clip_states` is a separate,
explicit user command. Ordinary editing, failed verification, load, replace,
save and close never discard unverified foreign metadata. Its boolean result
means the slot accepted the command, not that every pending source matched;
inspect the pending list and returned warnings for that result.

Catalogs are verified through core `restore_catalogs`, sharing prefix hashes and
transaction/frame checks across a batch. Coalesced catalogs receive one further
batch check. Duplicate file tabs with the same cached full source identity share
the prepared projection during a command. This avoids one history scan per
catalog or per draw. Allocation/cancellation failures propagate before metadata
publication rather than being labeled as a foreign recording.

Metadata is shared by source-path archives, like bookmarks. Editing one tab
updates other tabs' cached catalogs and revisions; a tab still holding different
source contents retains its own verified/pending classification. Closing and
reopening keeps the archive. Replacing same-path content keeps old mismatching
catalogs pending. `settings()` writes those pending values too, including when
the capture has no open tab. A stopped live source's `bind_source` transfers its
temporary clip values into the completed recording's archive without replacing
the Session or retargeting another active tab. Existing path-archive conflicts
are preserved. Unrecorded transient slots remain excluded from disk settings.

The content-only clip panel emits `ClipPanelChange{source_slot, kind}` only after
successful commands. `edited` and `verified` use the normal setter; only
`discarded` authorizes the host to call `discard_pending_clip_states` first.
Save As and pure drawing do not emit this callback. A host must not replace the
panel's catalog map or its Document during the callback: store values and update
the revision marker, then synchronize external revisions before the next draw.
Save As continues to retain its copied source slot, validate before/after the
destination chooser, and freeze that same Session for the existing ExportJob.

## Explicit persistence and strong restore

```cpp
Workspace workspace;
auto slot = workspace.add(std::move(loaded_document), absolute_utf8_capture_path);
workspace.save_file(workspace_path);          // default: do not overwrite
workspace.save_file(workspace_path, true);    // host explicitly authorized replacement

auto pending = workspace.load_file(workspace_path);
for (auto id : pending.source_slots) {
    // Queue an asynchronous load for workspace.find(id)->source_path().
}
```

Loading validates the complete sidecar before replacing workspace state. It creates source placeholders with pending verified views; it does not open captures on the UI thread. After the host loads each source, replacement checks its actual fingerprint before applying the pending view. Missing or changed captures remain recoverable as placeholders/unverified metadata. Locks reject whole-workspace replacement. Parse errors, unknown versions and failed validation leave the previous workspace unchanged.

The sidecar contains open file-source slots, their current view/camera/category options/render and inspector preferences/identity, per-path bookmark archives and latest matching views, recent files, global default preferences, and the active persisted slot. It embeds no raw capture, resource mesh, receiver state, export job, credentials, or hidden-object state. Transient slots are omitted; the host can explain this when saving a workspace containing unrecorded live data.

The app-owned format uses an eight-byte magic, u32 version, u64 payload size,
CRC-32 and a reserved zero field. The current writer produces version 5. Its
preference record retains the 62-byte v2 prefix and the 17-byte v3 navigation
record, then appends visualization scopes, actor/shape/mass-frame switches,
coordinate-frame scale, transparency opacity and the Inspector integer-hex
boolean. Each stored view also appends sleeping/trigger scopes before its
preference record. V5 retains that v4 prefix, appends face-culling and marker
preferences, and adds geometry/plane/joint-size fields plus ten RGBA palette
entries and the profile event-color flag to ViewOptions. A complete global
ViewOptions follows global ViewPreferences. Each capture archive appends
its bounded catalog list after bookmarks. Clip bindings and ranges use exact
fixed-width u64 IDs, generations, event counts and frame ordinals. Native
padding is never serialized.

Versions 1/2/3/4 remain readable. V1 gets right-handed display and default
preferences; v1/v2 receive built-in navigation defaults. Older versions get
default All scopes for existing master switches, no new coordinate frames,
transparency None and integer hex off (v1–v3); pre-v5 files get geometry/joint
scale 1, planes on, culling None, marker off and the original palette. No older
version invents clips absent from that format. Fixed independent
524-byte v1, actual old-writer 491-byte v2 and actual 04c3055 v3-writer 558-byte
fixtures and actual frozen 0.8 v4-writer 1,018-byte output are retained.
Integers and IEEE float bits are little endian; strings
are length-prefixed validated UTF-8 without NUL. Booleans, enums, IDs, counts,
identities, navigation, visualization domains and camera math are validated.
Integers never pass through JSON/double. CRC detects accidental damage, not
authenticity. Unknown versions or identity algorithms fail explicitly.

Default limits are 32 open slots, 256 capture archives, 32 recent files, 1,024
bookmarks per capture, 64 clip catalogs per capture, 1,024 clips per catalog,
4,096 clips across one capture archive, 256 UTF-8 bytes per clip name, 1 MiB per
other string, and 16 MiB per file. Pending catalogs count against the same
budgets. Collection counts, remaining bytes and clip-name limits are checked
before allocation. Limits may be reduced. Budgets fail explicitly; bookmarks
and foreign clip metadata are not silently evicted.

A save validates and encodes first, exclusively creates a private sibling, writes/flushes/closes it, then atomically publishes it. Existing destinations are refused by default, including a file that appears during publication. Explicit overwrite uses replacement publication. Failure removes the private file and preserves an existing destination. Windows uses wide UTF-8 path conversion and `MoveFileExW`; POSIX uses exclusive `open`, `link` or `rename`. No destructive delete-then-rename gap is used. A process crash can leave an identifiable `.pvdws-tmp-*` sibling; directory-entry durability across power loss is not guaranteed.

## Verification

`tests/workspace_tests.cpp` uses real core Sessions and Document render/selection state. It verifies pointer stability through slot-vector growth, per-slot selection/frame/camera, locks and ownership, live updates continuing in the original slot after activation changes, and stopped-source binding without moving Session identity. It tests a same-path/same-event-count geometry change, per-bookmark identity verification across both old and new open Sessions, persistent placeholder restore, and canonical identity across wire repacking.

Codec/file tests cover maximum/high-bit 64-bit IDs and frame values, UTF-8 names and filenames, exact arbitrary-up camera doubles, view options, every byte truncation, CRC damage, unknown version, hostile collection counts, invalid UTF-8/camera/enum/preference data, ID exhaustion, bounded reads, no implicit raw loading, default overwrite refusal, authorized atomic replacement, and failed-load strong protection. Preference tests cover exact float-bit round trips for every current field, per-slot/global-default isolation, matching versus changed-source restore, archive reopening, stopped-recording binding, camera-only bookmarks, all validation domains, hostile CRC-correct enum/boolean/NaN/page payloads, file budgets and unchanged destinations after rejected saves. The prior identity-quota and complete-camera bookmark collision audits remain covered.

V4 tests additionally encode/decode and actually save/reload active plus foreign
pending clip catalogs, restore placeholders, replace same-count changed sources,
synchronize duplicate tabs, close/reopen archives, import conflicting stable IDs,
promote a future seal only after explicit verification, and associate a stopped
live Session with an existing recorded archive. They check all three named
variants survive that merge, original live seals remain unchanged, ordinary
setters preserve pending metadata, explicit discard removes it, and budget or
structural rejection leaves values, caches and revisions unchanged. Correctly
checksummed hostile clip counts and invalid clip UTF-8 are rejected. Actual
1x/1.5x/2x ImGui command tests verify change kinds, no callback on draw/Save As,
and no persistence notification after failed verification.


## Native application host

`viewer/main.cpp` owns a `Workspace` and uses stable IDs for queued `LoadJob` results, live import, raw saves and RepX exports. Each slot also retains independent hierarchy/inspector filters, property curves and diffs, event curves, analysis indexes, scene tools, debug render switches and camera tracking. Switching tabs preserves these transient states; replacing that slot resets them. Disk settings persist source-backed cameras, category visibility/colors, full render/inspector preferences, global defaults, frames and bookmarks. Search filters, curves and analysis UI state remain transient.

Opening a capture creates a separate asynchronous load target. The loader reports actual bytes, file size and accepted event counts, then prepares the first scene and computes the canonical identity on its worker. Cancellation or failure leaves a replacement target's previous Document intact. Sources restored from a workspace are queued without retargeting when the active tab changes. Closing the final unlocked tab creates an empty fallback. Locked tabs report why close or replacement is unavailable.

Live import always looks up its original slot. `Show live tab` returns to that source. A save or RepX job transfers that source's live lock to an export lock and pauses only UI import while the worker borrows the Session; TCP receive and raw recording continue. Other slots remain usable. Completed recording association fingerprints the original immutable Session on a worker; multiple associations queue with locks, and `bind_source` preserves the original Session address. A finished live tab can be closed without leaving the receiver pointing at a missing slot.

Normal visible application runs save their source-backed workspace under `%LOCALAPPDATA%/PvdStudio/workspace.pvdws`, with ImGui layout in the adjacent `layout.ini`. A damaged or unknown workspace disables automatic replacement and shows an error. `--no-settings`, `--hidden` and every smoke mode disable automatic reads and writes; no current-directory `imgui.ini` is created. Workspace menu commands provide explicit Save/Load, recent sources, bookmarks, Reset layout and shortcut help. `Ctrl+B` records a frame and camera; restoring such a bookmark stops tracking. Text editing, active controls, focused keyboard widgets and popups suppress conflicting global playback shortcuts.

The `View...` popup exposes recorded cameras, tracking, category colors/visibility and independent render layers. Camera tracking updates after playback/seek and before rendering. Rigid and native deformable picking compete by nearest ray distance and respect visible render layers. RepX is strict by default; an explicit partial export retains its diagnostics in XML and a visible host status.

`tests/viewer_workspace_smoke.py VIEWER CAPTURE FIXTURES` uses the real Win32/ImGui/D3D host, asynchronous loads and writer to verify two independent slots, unchanged load targets after switching, lock protection, retained curves, inactive-source export and file reopening, exact saved-view restoration and verified bookmarks. The capture needs at least five native frames and a dynamic actor pose. Outputs are unique under the supplied fixtures directory. `--size 1060 700` and the default 1440 by 900 were visually checked with the actual GPU output. `--live-smoke` additionally changes active tabs during real SDK TCP reception and asserts that import stays in the original slot while a concurrent safe snapshot is written.

Additional diagnostic entry points are `--analysis-smoke CAPTURE --page event-curves`, and `--repx-smoke CAPTURE --output NEW.repx [--allow-partial]`, with optional `--warp --hidden --report RESULT.json --screenshot IMAGE.png`. Strict RepX failures return an error without publishing XML; partial results report `complete:false`, the opt-in and diagnostic count. Native-frame raw cropping remains one-based in its UI and CLI. The `--workspace-smoke` entry requires an unused `--workspace-state NEW.pvdws` destination and reports exact behavioral assertions rather than a synthetic UI mock.
