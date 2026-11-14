# View display settings

The content-only `drawDisplaySettings` panel controls the viewport background,
antialiasing request, signed up axis, display handedness, and joint frame/limit
visibility. The host owns its window/popup and passes the last rendered
`RenderStats`. Each viewport uses its own `OrbitCamera` and `RenderOptions`.

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
