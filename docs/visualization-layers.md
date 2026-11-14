# Visualization scopes, local frames and transparency

The display options support **None / All / Selected / Unselected** independently
for sleeping rigid bodies, trigger attachments, tight bounds, linear/angular
velocity, scene-query graphics and local coordinate frames. Existing `show_*`
booleans remain master switches. New scopes
default to All, preserving earlier saved-view behavior when their master is on.

Selection uses the current snapshot's real generation-aware keys. A shape
selection with an attachment key remains specific to its owning actor, including
shared shapes. Explicit actor inspection selects that actor's attachments.
Articulation inspection follows native link `Parent` relations; aggregate
inspection uses recorded `Actors` and `Articulations` membership. Missing,
destroyed or different generations are not retargeted. Parent walks are bounded
to the native articulation depth and owner collections have a finite budget.
Hidden attachments are always excluded before inspecting overlay properties.

Sleeping and Trigger controls are in Scene tools. Their scopes are independent
additional visibility filters, rather than replacing Static/Dynamic/Kinematic
categories. Sleeping None removes only actors whose captured sleep state is
true; awake actors and unavailable sleep states keep category visibility.
Trigger None removes only captured trigger attachments. If both filters apply
to an attachment, both must pass. An inspected actor/articulation/aggregate
selects its real attachments, while a picked shared-shape attachment selects
only that actor/shape pair. The common UI-free selection helper lives in
`model.hpp`/`model.cpp` and is also used by overlay scopes and transparency.

These filters update immediately after selection, inspection, clearing,
forward/reverse seek and safe live-frame publication. Hidden/isolate rules
remain additional constraints. Restore all and the existing Isolate commands
restore category masters and both status scopes to All. A retired attachment
does not turn into an all-owner selection merely because its shared shape
still exists; actor/scene/shape generation mismatches cannot retarget it.
Changing visibility never changes source flags, sleep state or poses.

Scene-query scopes use the exact selected query row's `SceneQuerySource`,
including scene generation, publication and query sequence. They do not infer
query ownership from source flags or invent a relation to an inspected actor.
With no selected query, Selected draws none and Unselected draws the recorded
query graphics. Query paths, normals and volumes retain their independent
master controls.

## Coordinate-frame data

`pvd::render::build_coordinate_frames` is a CPU-only helper in `pvd::render`; it
links neither SDK, ImGui nor GPU APIs. Its independent actor, shape and mass
families each emit three colored positive axes: X/red, Y/green, Z/blue.

| Family | World transform |
| --- | --- |
| Actor | Captured Actor.GlobalPose, read through `coordinates::native_transform` |
| Shape | Extractor's native `ShapeInstance.world_pose` for that actual attachment |
| Mass | Native actor transform composed with captured local `CMassLocalPose` |

Actor and mass frames deduplicate by scene and actor generation. Shape frames
remain per attachment. Dynamic bodies and articulation links have mass frames;
statics are skipped. Missing dynamic COM pose produces a diagnostic instead of
falling back to the actor's frame. OriginShift affects retained native world
poses through the core coordinate reader; it is not applied a second time to
local COM or already-native shape poses. Source properties, poses and events
are never rewritten by display code.

The helper checks matching Frame/Snapshot prefixes, source generations/classes,
captured scene mismatches, exact typed transform payloads, finite coordinates
and normalized quaternions. Options bound examined attachments, retained
diagnostics and the total line count shared by all three families. A triad is
committed atomically; an insufficient budget never emits half a coordinate
system. A present empty attachment mask or zero axis scale performs no source
property inspection. Failure remains visible through diagnostics and the
renderer exposes both the result and completion/count status.

## Actual alpha rendering and picking

`transparent_scope` defaults to None. Matching mesh fills and wire overlays use
`transparent_opacity` in `[0,1]`, multiplied by the view's existing color alpha.
Selected coloring preserves this effective alpha. These are view overrides,
not physics material or captured shape-flag changes. Debug/coordinate overlays
keep their own layer controls.

Opaque geometry writes depth first. Transparent fills are sorted back-to-front
by their world-bounds center (pose position when no finite bounds exist), tested
against opaque depth and alpha blended without depth writes. Wire overlays also
read depth without writing it. Sorting is per attachment; intersecting surfaces
and triangle order within a complex mesh are not claimed to be order-independent
transparency. This is a real D3D pass, not a UI opacity value with opaque draws.

Effective zero-opacity geometry submits neither fill nor selection wire.
`visualization_pick_draws` applies the same visibility/opacity policy to CPU
shape candidates, allowing a ray through an invisible front attachment to hit
an actual visible back attachment. The host passes the same transient
`VisualizationSelection{inspected_object, selected_attachment}` to rendering and
this adapter. This context is not a persisted preference or an assumed stable
pointer/index identity.

The display panel additionally exposes particle/cloth/debug/contact marker
size in screen pixels and contact-normal length in world units. Those controls
do not scale source positions or normals. All fixed layout sizes use the scoped
UI pixel helper; the long content remains operable by scrolling at narrow widths
and high DPI.

## Validation

- `status_scope_tests`: independent Sleeping/Trigger None/All/Selected/Unselected
  controls, legacy boolean masters, awake and unavailable state, exact shared
  attachments, actor/scene/articulation/aggregate owners, hidden/isolate rules,
  reverse seek, live wake/flag updates, recycled actor generations, invalid
  scopes, immutable published frames and source fields. Real ImGui mouse input
  exercises both combos at 1x/1.5x/2x with logical width 420. An optional real
  SDK recording is checked at native and after-fetch prefixes in every scene;
  local runs compared 180 articulation and 272 geometry-fixture attachments.
- `coordinate_frames_tests`: native and retained OriginShift values; nonidentity
  actor/COM composition; shape poses; dynamic/link/static behavior; shared-actor
  deduplication; attachment masks; source scene/class/generation failures;
  reverse seek; malformed and missing poses; atomic budgets; LE/BE and 32/64-bit
  sources. Optional arguments are the real articulation capture and SDK getter
  CSV. The local SDK run compared 1,560 axes forward/backward using independent
  matrix composition of the public getter values.
- `visualization_layers_tests`: actual D3D11 WARP output checks independent
  selected/unselected scopes, shared-shape specificity, owner graphs, hidden and
  stale-generation behavior, query-source selection and missing COM diagnostics.
  Actual alpha pixels remain identical when input attachment order reverses;
  opaque occlusion and read-only transparent depth are verified. Zero-opacity
  surfaces emit no mesh/highlight pixels and cannot intercept the CPU ray.
- The same WARP test optionally consumes the real articulation recording and
  verifies selected bounds/velocity/native COM, unselected transparency, hidden
  source exclusion and unchanged pose bytes. Actual ImGui controls are operated
  at 1x/1.5x/2x, logical 420x700, including scrolling to lower settings.

No runtime control reconstructs contacts, query transport, velocity or COM data
that was not captured. Empty uncollected channels and malformed requested poses
retain their existing availability/diagnostic boundaries.
