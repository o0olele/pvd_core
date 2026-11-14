# PhysX 3.4 constraint display lines

`pvd::render::build_constraint_lines` converts the five typed `DebugConstraintPayload` alternatives into the native line primitives emitted by PhysX 3.4's `CmVisualization`. It depends on core/render declarations only; it does not link PhysX, a window system, ImGui or a GPU. All output is derived display geometry, with the original `DebugSource` and exact SDK ARGB endpoint colors. It never changes recorded limits or instance state.

The implementation is based on the local SDK's `PhysX_3.4/Source/Common/src/CmVisualization.cpp`, `CmRenderOutput.cpp`, `CmVisualization.h`, and `Include/PxConstraintDesc.h`. The NVIDIA/AGEIA/NovodeX BSD notice is retained at the top of `src/constraint_builder.cpp` and in `docs/constraint-visualization-LICENSE.txt` for binary documentation. The original SDK implementation is called separately by the optional validation executable rather than copied into its oracle.

## API and conventions

```cpp
render::DebugOptions debug_options;
debug_options.joint_axis_length = 0; // avoid the older separate axis overlay
const auto raw = render::extract_debug(session, native_frame, debug_options);
render::ConstraintBuildOptions options;
options.frame_scale = 0.25f;
options.limit_scale = 0.5f;
const auto display = render::build_constraint_lines(raw.constraints, options);
// Append display.lines to this frame's debug lines; keep display.diagnostics.
```

Input poses are already world-space. No actor/local pose, scene origin shift, handedness conversion, or scale is applied a second time. World display scale is supplied explicitly because these PVD events do not record the immediate visualizer's `mFrameScale` or `mLimitScale`. The API default is one world unit; zero disables the corresponding family. Parent frame axes are 1.5 times the requested frame scale and use the SDK dark red/green/blue colors (`0x88880000`, `0x88008800`, `0x88000088`). Child axes use the requested scale and full red/green/blue. Limits use red when active and grey (`0xff808080`) when inactive.

| Typed payload | Native lines | Meaning |
| --- | ---: | --- |
| `DebugJointFrames` | 6 | Parent and child world axes. |
| `DebugLinearLimit` | 21 | Signed distance along `t0`'s x basis plus a 20-segment circle of radius `0.3 * limit_scale`, perpendicular to that axis. `t1` is preserved/validated but the SDK algorithm does not use it. The signed distance is not multiplied by display scale. |
| `DebugAngularLimit` | 22 | Two radial boundaries and a 20-segment arc in the local yz plane, using the recorded lower/upper radians. |
| `DebugLimitCone` | 66 | The SDK's 32-subdivision quaternion swing boundary and origin spokes. The first origin-to-boundary segment is deliberately repeated, as in the SDK. |
| `DebugDoubleCone` | 128 | Two 32-segment rings at local x coordinates `+/-tan(angle) * limit_scale`, with 64 origin spokes. |

The misleadingly named wire `DebugLimitCone.y_swing` and `z_swing` are **tangents of quarter angles**, not angles in radians. The SDK constructs a normalized quaternion from `(0, -z_swing*sin(theta), y_swing*cos(theta))`, then rotates the local x direction. `ExtSphericalJoint.cpp`, `ExtD6Joint.cpp` and `NpArticulationLink.cpp` are the source-side callers. The builder consumes their serialized argument order without swapping parameters again.

The line ordering, seam closure, active flag colors and SDK float-angle stepping are retained. `CmRenderOutput::DebugArrow` additionally emits solid arrowhead triangles; this line-only API exposes its original shaft line and does not invent triangle edges or claim to reproduce those solids. It does not generate contact forces, joint error forces, or unrecorded solver information. Input is typed, so source pointer width and wire byte order are handled by the decoder and do not depend on the consumer ABI here.

## Validation and budgets

Both display scales must be finite and nonnegative. `max_diagnostics` must be nonzero; invalid options throw `std::invalid_argument`. `max_constraints` and `max_lines` may be zero. Input poses must have finite position and quaternion components and satisfy the SDK `PxQuat::isUnit` tolerance: `abs(magnitude - 1) < 1e-4`. Quaternions are never normalized or repaired. Even the unused linear `t1` is validated.

A linear distance may be negative. Angular bounds are finite, ordered `lower <= upper`, with a finite difference; negative bounds and locked/equal bounds are preserved. They are not wrapped, sorted or converted from degrees. Cone tangent arguments are finite in `[0,1)`, corresponding to nonnegative swings strictly below pi; zero retains the SDK degenerate boundary. Double-cone angles are radians in `[0,pi)`, with a finite tangent. Near the tangent pole a finite recorded float can produce very large finite geometry, just as in the SDK; no hidden clamping is applied. Nonfinite derived coordinates, including overflow from otherwise finite inputs or scales, reject the entire affected constraint.

Default budgets are 1,048,576 examined constraints, 1,048,576 lines and 256 retained diagnostics. Generation has fixed work per family (at most 128 lines) and stages one complete constraint before appending it. An invalid payload commits no lines and reports its original source and input index; later valid constraints may still render. A work or line budget stops at a complete constraint boundary. `complete` becomes false for every invalid/budget failure even after the diagnostic retention budget is full. `constraints_processed` counts examined records, including rejected/disabled records; `constraints_emitted` counts records that committed geometry. Budget exhaustion does not silently draw part of an axis set or cone.

The function does not infer a frame interval, renderer ownership or scene from historical packets. Pass only constraints selected for the intended frame/range by `extract_debug`. Do not concatenate every historical frame into a display result. Scene attribution must use recorded ownership evidence; world-anchored joints may use their non-null actor, and an unrelated renderer must not become scene-owned merely because it emitted a constraint.

## Verification

`tests/constraint_builder_tests.cpp` is SDK-free. It checks all five exact native line counts, SDK active/inactive and parent/child ARGB colors, source provenance, world translation/rotation, parent 1.5 scaling, signed linear offsets and unscaled distances, the quarter-angle tangent interpretation, both cone rings/spokes, zero boundaries/scales, non-unit/nonfinite poses, bad radians/tangents, finite-input output overflow, diagnostic caps and whole-constraint budget behavior.

`tests/physx34_constraints.cpp` is an optional Windows x64 Debug validation executable. It links the supplied **actual** `PhysX3CommonDEBUG_x64.dll` and calls the exported `Cm::visualizeJointFrames`, `visualizeLinearLimit`, `visualizeAngularLimit`, `visualizeLimitCone`, and `visualizeDoubleCone` into SDK `Cm::RenderBuffer` instances. It compares every line endpoint, line order and exact color with this independent implementation. It also checks source provenance. The synthetic oracle covers both identity and translated/rotated poses, active/inactive flags, scales 0.25/2, signed/zero linear values and double-cone angles on both sides of pi/2 (56 SDK comparisons).

The real `joints_capture.pxd2` fixture, generated by `examples/physx34_joints_capture.cpp` with constraint transmission enabled, contains 11 typed constraints: six frame pairs, two linear limits, one angular limit, one cone, one double cone. Their 294 native lines match the actual SDK DLL. The oracle additionally verifies native-frame scene/range attribution without history duplication. The independent build instructions and logs are in `analysis/constraint-sdk-oracle`; normal library and unit tests do not require an SDK checkout.
