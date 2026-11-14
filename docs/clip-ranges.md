# Named native-frame clips

The installed, SDK/UI-independent `pvd/clips.hpp` API in `pvd::core` adds a non-destructive
clip catalog over `pvd::Session`. A catalog retains multiple named ranges,
stable IDs and a selected range. Editing never removes source events or changes
the source playback timeline. The content-only ImGui panel is
`viewer/clip_panel.hpp` / `.cpp`; its host supplies the source slot and Save As
callback.

It is available when `PVD_BUILD_VIEWER=OFF`. `src/clips.cpp` owns the implementation;
the viewer header is only a compatibility include. `restore_catalogs` verifies
multiple saved catalogs with shared prefix hashing and transaction-boundary
scans. It returns one validation result per input; cancellation and allocation
failures propagate instead of being misreported as a foreign recording.

Ranges use inclusive, zero-based **original native-frame ordinals for a scene
ID**, including any earlier lifetimes of that ID. The UI displays one-based
numbers. They are the same indices consumed by `ExportRequest` with
`ExportMode::native_frame_range`, rather than global `Session::frames()`
indices. Each catalog is additionally bound to the exact scene generation.
Ranges cannot cross a scene's destruction/recreation. Other scene/stream
events in the selected global interval remain preserved by the existing writer.

`Catalog::create` starts with one selected `Scene range` covering that scene
lifetime's closed frames. `add` retains another independently named range;
`rename`, `erase` and `select` operate on stable IDs. Deleted IDs are not reused.
`split(playhead)` replaces the selected range with two ranges: the playhead is
the final frame of the left side and the right starts at the next frame. The
left side is selected. Splitting the final frame or producing an empty side is
rejected. Trim left/right keep the playhead, preserve the selected ID and may
produce a valid one-frame clip. These rules are explicit rather than guesses
about the old application's undocumented boundary convention.

Names are bounded, nonempty UTF-8 text, including literal `##` / `###` strings.
Malformed names, invalid selections, duplicate IDs, overflowing/out-of-bounds
ranges, altered source seals and exhausted budgets fail before publishing an
edited catalog. A cancellation exception from `Limits::checkpoint` has the same
strong guarantee. The default budget is 1,024 clips and 256 name bytes.

## Source identity, live capture and persistence

Each `Clip` contains its own `Binding`: scene ID/generation, original available
frame bounds and a sealed source identity. The identity hashes the header and
the canonical wire events, including original stream IDs and timestamps. Its
algorithm 1 matches `workspace_source_identity` for the same complete Session;
byte offsets and file paths are not identities. A same-length replacement with
different contents fails validation. The FNV identity is intended to detect
changed recordings, not to serve as a cryptographic security signature.

Live capture is supported. A catalog seals the latest transaction-safe event
prefix, or an explicitly requested safe prefix. Only already closed native
frames in that prefix are available as range endpoints. An open frame or a
pending chunk/message-group tail cannot become a range end. `validate(Session)`
checks the **stored prefix**, allowing subsequent Session append without making
existing clips invalid. `refresh` explicitly admits newly closed frames; it
does not rewrite any old range or old range's seal. A new range can use the new
seal. Ordinary open sections do not prevent transaction-safe prefixes.

`State` contains the current binding, clip values, selection and next stable ID
for the host's workspace persistence. Restore with `Catalog::restore(session,
state)`; it validates content, source lifetime, frame ordinals, names and limits
before publishing the catalog. The panel keeps separate catalogs by
`SceneKey`, so selecting another scene or a reused ID does not delete previous
fragments. A changed Document source disables archived commands until explicit
verification; failed verification leaves those clips intact for review or
explicit discard. Verification happens on commands/restore, never every draw.
Within a catalog, source seals are hashed in one forward pass and frame and
transaction endpoints are shared across ranges rather than rescanning history
once per clip.

## Host integration

```cpp
ClipPanelActions actions;
actions.busy = source_is_locked_for_replace_or_export;
actions.save_as = [&](ClipSaveRequest request) {
    // Resolve request.source_slot, never a possibly changed active tab.
    // Validate against that source before and after the destination chooser.
    // Lock/freeze that source before starting its existing ExportJob.
};
drawClipRanges(source_document, stable_source_slot, state, actions);
```

`save_as` receives a **copy** of the source slot and complete clip binding/range.
The host validates it with `clips::validate_clip`, obtains a destination, retains
the same slot/Session, and dispatches the existing background `ExportJob`. It
sets `scene_id`, `first_native_frame` and `last_native_frame` from the clip.
The panel does not start a worker, choose a file, close a slot or resolve a
global active document. Live reception can continue; the host freezes the
source's imported view while its existing export job borrows the Session.
Writer cancellation, strict/partial analysis bootstrap and atomic publication
retain their existing behavior.

Playhead split/trim require the catalog's exact scene lifetime and the **Frame
end** timeline. An after-fetch sample is not silently treated as a native
frame. `Select range` explicitly switches to the clip's native scene/timeline
and first original ordinal. Selection in the clip list changes the active clip
without silently changing the playback sample.

The main window has a Clips tab. Ctrl+Alt+Y splits, Ctrl+Alt+S trims left and
Ctrl+Alt+E trims right at the native playhead; text input and open popups retain
their keyboard input. Workspaces retain verified catalogs and separately keep
unverified foreign-source archives until an explicit discard command.

## Validation

`viewer_clips_gpu` also runs the actual WARP-backed main window, performs split,
both trims, rename/add/delete, exports the original source while another tab is
active, reopens its six-frame native clip and restores the saved workspace.
It verifies stable IDs, source seals and names, native end state and both visible
list rows. The smoke supplies a destination directly; the operating system's
file chooser remains a separate manual interaction check.

`tests/clip_ranges_tests.cpp` links only the clip value model and core. It covers
LE/BE, interleaved scenes, reused scene IDs, split coverage without duplicate or
missing frames, both trims, stable delete/add IDs, persistence validation,
UTF-8/budget/cancellation failures and an actual writer crop/reparse preserving
64-bit values. It also tests live append, open frames, unfinished transaction
tails, explicit refresh and same-count changed sources.

`tests/clip_panel_tests.cpp` runs actual Dear ImGui table selection and buttons
at logical width 1,060 with 1x/1.5x/2x fonts/style, plus width 420 at 1x. It
exercises named split/trim/add/delete/rename, native range navigation, source
slot and busy-lock rules, rejected verification and retry, independently
retained scene/generation catalogs, live sealed-prefix Save As dispatch, and
canonical identity compatibility with Workspace. Its captured Save As request
is sent through the real `ExportJob`, saved to a unique file and reopened with
the expected native frame count and exact final integer property. It accepts
an optional temporary output directory and removes only its own generated file.

These tests exercise content-only widgets and the actual data/export APIs.
Physical system mouse/file-dialog interaction and final host integration are
separate application acceptance checks; a passing panel test does not claim
those interactions were performed.
