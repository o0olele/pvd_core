# View display settings

The content-only `drawDisplaySettings` panel controls the viewport background,
antialiasing request, signed up axis, display handedness, geometry display scale,
planes, face culling, orbit-target marker, and joint frame/limit sizes and
visibility. The host owns its window/popup and passes the last rendered
`RenderStats`. Each viewport has an `OrbitCamera`, `RenderOptions`, and the
Document's `ViewOptions`. These are display preferences, independent of capture
decoding, physical simulation and exports.

`ViewOptions::geometry_scale` is a positive uniform **display-only** scale in
`[0.01,10000]`, default 1. Each shape is scaled about its own local origin after
the recorded mesh scale has been baked, before its recorded world pose. Shared
shapes retain exact attachment identity and each owning actor's transform.
The mesh resource and `DrawItem::world_pose` are unchanged; `display_matrix`,
`display_point` and `display_bounds` provide the common display transform.
GPU fill/wire/highlights, CPU triangle picks, scene/selected fit, transparency
sort centers and the **Display geometry bounds** overlay use it. At scale 1,
source finite bounds and the previous finite plane-patch fit policy are retained.
Changing scale does not rebuild/upload mesh buffers. Bounds/vertices outside
representable float coordinates fail explicitly rather than clipping silently.

The source snapshot, physical/native geometry and bounds in `FrameState::render`,
actor/shape poses, contact points, debug primitives, velocities, cloth/particle
positions, scene-query geometry and RepX output remain unchanged. Those layers
may intentionally extend beyond a shrunken visual shape. A shape pick reports
the intersection with the displayed surface; the selected source IDs still
refer to the original attachment. Source values remain available in Inspector.

`ViewOptions::show_planes` hides native plane attachments through the same
logical visibility used by picking, fit, overlays and visible-scope export.
It does not remove independent captured debug triangles or change an infinite
physical plane into a finite resource. **Restore all visibility** restores this
category along with the other category switches.

`RenderOptions::face_culling` is None, Front, or Back (default). Back-face
culling exposes the interior of inward-facing collision meshes: the reverse
side of a ceiling or near wall is invisible from outside, while front-facing
walls, floors and closed solid geometry remain opaque. This is a cutaway by
triangle orientation, not alpha transparency or automatic removal of every
roof. Choose None to inspect both sides, or Front to inspect reversed faces.
The setting is under **Display -> Display and camera settings -> Display**.
It applies to
shape fill, wire/highlights and CPU face picking. Front follows the triangle
buffer's original winding; a horizontal display mirror changes the GPU's
front-face convention so the same physical side remains front in both hands.
The host passes `visualization_pick_draws(frame, options, selection)` into CPU
`pick`. Debug/point markers and native deformable constraints remain independent.
Culling does not delete a source attachment or make visible-scope export a
camera-frustum selection.

`RenderOptions::show_orbit_target`, default false, draws a small screen-sized
cross at `OrbitCamera::target`. It is a depth-independent navigation overlay,
including when the target lies inside geometry. It is not a source object,
does not intercept picking, and does not influence fit or export.

`OrbitCamera::reference_up` accepts recorded arbitrary up vectors. The six
`DisplayUp` choices set it to ±X, ±Y, or ±Z while retaining target, distance and
orbit angles. A recorded tilted/rolled camera remains marked `Recorded/custom`
until the user chooses a canonical axis. The grid lies in the plane through the
world origin perpendicular to this up vector, centered around the target's
projection into that plane. Colored X/Y/Z axes still identify the actual source
world axes; choosing -Z up therefore shows source +Z pointing down.

`DisplayHandedness::left_handed` is a horizontal **display mirror**. It changes
the view basis, not the capture's coordinate system. Physical camera position,
world poses, raw vertices, native cloth/particle coordinates, source IDs and
RepX output are unchanged. Projection, screen rays, CPU shape/native picking,
panning, orbit direction, frame fitting, tracking, and recorded look-at cameras
use the same display convention. D3D front-face winding also changes so the
mirror preserves two-sided face lighting and selection overlays.

`OrbitCamera::screen_ray` carries a `RayViewClip` with the camera's forward
direction and near/far **view depths**. Rigid picking converts those planes to
the normalized ray's distance interval, so an off-axis ray is not incorrectly
cut at the same Euclidean distance as the center ray. It tests every triangle:
when the near plane removes a front surface, an eligible back surface may still
be selected, subject to the chosen face culling. Native cloth/particle markers
use their actual center view depth, rather than its projection onto a nearby
ray. Their angular selection tolerance cannot admit a clipped center or reject
one on the opposite side of the same ray. Markers follow the renderer's strict
near and inclusive far center thresholds.

Manually constructed two-field `Ray{origin, direction}` values retain the old
unbounded policy. Optional `min_distance`/`max_distance` constrain normalized
world-ray distance independently of camera clipping. NaN/negative/reversed
intervals and invalid view-clip directions are rejected explicitly. These are
viewer query policies; capture poses, resource bytes and export remain unchanged.

`RenderOptions::background_color` is a finite RGBA value in [0,1]. The panel
edits RGB. `Antialiasing::off` requests one sample; `msaa4x` requests four.
The renderer checks actual RGBA8 color, D24S8 depth, and color-resolve support.
All three must support the path; otherwise it uses one sample and reports
`fallback=true`. The UI distinguishes an intentional 1x request, a pending
configuration change, and a 4x request that fell back to 1x. It never labels a
request as actual support before a viewport render reports it.

With 4x enabled, drawing uses a multisampled color target and matching depth
buffer. After drawing, color resolves into the single-sample RGBA8 texture
returned by `texture()` and `shader_resource_view()`. This keeps ImGui sampling
and screenshot readback compatible. The separate readback helper also resolves
actual multisampled textures supplied by the host. Resize and antialiasing
changes preserve cached mesh buffers and recreate only viewport targets.

Typed native joint constraints remain in `FrameState::debug.constraints` with
their original source. `build_constraint_lines` supplies separate
`joint_frame_lines` and `joint_limit_lines`, sharing one configured total line
budget. The model disables the old extra joint-axis conversion to prevent
duplicates. A legacy not-tessellated diagnostic is removed only for a constraint
the builder actually handles; other source diagnostics remain. Builder failures
are visible in `FrameState::errors`, and partial display is not labeled complete.
Independent renderer switches hide frames and limits without changing source
events. These derived lines do not fabricate joint shapes or physical geometry.
`ViewOptions::joint_frame_scale` and `joint_limit_scale` are independent
nonnegative multipliers in `[0,10000]`, default 1, applied to the configured
constraint builder sizes. They rebuild typed display primitives at their own
joint origins rather than scaling world-space endpoints about the scene origin.
The recorded linear-limit distance and joint parameters remain unchanged. Zero
disables that derived family. Only a new sample or a size change rebuilds the
cached lines; ordinary color/selection changes reuse them.

The palette's native cloth/particle alpha participates in rendering and picking,
with captured per-owner colors retaining priority. Zero alpha excludes that
owner, including selected/pinned highlights and constraints, from drawing and
native-element picking. An invisible foreground owner cannot intercept a
visible fluid/rigid behind it. Fit uses logical owner visibility and captured
bounds, even if palette alpha is zero; this permits fitting an owner before
restoring its color. Source color bytes remain unchanged. See [palette](palette.md).

Derived display failures are transactional. File scene/sample/frame changes and
playback retain the previous cursor, timeline, selection and immutable frame if
the requested displayed coordinates overflow. After lowering scale, the same
operation can be retried. During live capture, a display-only overflow pauses
Follow and playback, retains the last published sample, and adds a visible
`FrameState::errors` diagnostic. Accepted source events and timeline entries
continue to accumulate; they are not replayed or rolled back and TCP ingestion
continues. Lower scale and enable Follow to publish the latest sample. Parser,
source-semantic and allocation failures are not converted into display success.

Workspace version 5 persists these options and the palette. Complete global
`default_view_options` apply to new slots; identity-matched per-source stored
views take priority, and changing defaults does not rewrite other open slots.
Versions 1–4 migrate with geometry/joint scale 1, planes on, culling None,
orbit marker off and the original palette. See [workspace](workspace.md).

`tests/display_settings_tests.cpp` checks user-visible behavior across six
signed up axes and both hands: full-bounds fitting, projection-to-pick agreement,
unchanged source world coordinates, pan direction, recorded roll, and forward/
reverse tracking. Actual WARP rendering verifies background pixels, genuine
1x/4x edge coverage, resolved SRVs, mirrored lighting, selected overlays,
grid/axis changes, odd-sized multisample readback and resize. Unsupported-device
selection is tested through the same capability policy, without pretending
that the tested WARP device lacks 4x support. Real Dear ImGui tests at widths
420 and 1060 exercise the antialiasing combo and pending/fallback status text.
An optional `joints_capture.pxd2` argument adds the real SDK oracle/model check
for all 294 native joint line segments and the shared-budget failure path.

`geometry_display_tests` adds actual WARP fill-area/cache checks, both triangle
windings × both hands × three culling modes with matching CPU picks, scaled
transparency-order changes, displayed bounds and the orbit marker. Real ImGui
mouse input controls geometry/planes/culling/marker/joint sizes at logical
420/1060 widths and 1x/1.5x/2x. Navigation overflow tests cover frame/step/scene/
sample/playback rollback, accepted live prefixes and recovery after lower scale.
The separate real-SDK display oracle validates resource/world coordinates,
fitting, joint origins and raw/RepX/query isolation against actual SDK captures.
