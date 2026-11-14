# Derived rigid-body display overlays

`pvd::render::build_body_overlays` produces optional tight AABB, linear velocity and angular velocity line layers from an existing render `Frame` and its captured `Snapshot`. It depends on core/render values only: there is no Metadata, Session, SDK, UI, window system or GPU dependency in the helper. The renderer can call it only when a requested layer is enabled; it is not a fixed part of core frame construction.

```cpp
render::BodyOverlayOptions options;
options.show_bounds = true;
options.show_linear_velocity = true;
options.show_angular_velocity = true;
options.velocity_scale = 0.2f;
options.visible_attachments = currently_visible_keys;
auto overlay = render::build_body_overlays(frame.render, frame.snapshot, options);
```

`visible_attachments == nullopt` selects all input Frame attachments. A present empty vector selects **none**, with no source traversal or diagnostics. The filter runs before property reads, so a hidden attachment cannot leak an arrow or errors about its missing fields. All flags off, or vector-only layers with scale zero, also return without traversing instances/properties or copying the mask. Options are passed by const reference so a large visibility list is not implicitly copied before its budget check.

## Source identity and captured data

Every output line has its real `PickingKey` (scene, actor and shape IDs and generations), exact snapshot event prefix, overlay kind/part and `derived=true`. The Frame prefix must match the Snapshot whenever visible geometry is processed. Scene/actor/shape instances must still match their recorded generations. A known foreign actor Scene reference is rejected. The existing `ShapeInstance.actor.kind` is the authoritative render extraction classification, so Metadata is unnecessary.

Bounds come directly from `ShapeInstance.world_bounds`, the render extractor's tight world-space geometry AABB. They are **not** SDK broadphase bounds, contact/rest offsets, swept bounds or collision inflation. Each visible unique attachment emits 12 complete edges. Shared shapes attached to different actors retain separate attachment keys and world bounds. Finite ordered zero-extent bounds remain flat; the helper does not add artificial thickness. Missing finite bounds, including infinite planes, are reported instead of manufacturing a box.

Linear and angular velocities use exactly one captured typed `PxVec3` from `LinearVelocity` or `AngularVelocity`. The anchor is captured `Actor.GlobalPose.position`, never a shape center, bounds center or estimated pose delta. The pose must be one finite typed transform with unit rotation. Velocities are already world-space and are not rotated by that quaternion. Static actors are skipped without guessing/looking for velocity fields. Dynamic and articulation-link actors use their captured properties, including kinematic/sleeping bodies when the values are available; no motion is inferred when fields are absent.

One vector per `(scene generation, actor generation)` is emitted, even with several visible shapes or repeated input attachment rows. The representative picking key is the first visible attachment in Frame order. Hiding that attachment lets another actual visible attachment become the representative. A shared shape attached to two different actors still produces two independent actor vectors. Raw IDs never pass through floating-point values.

## Display geometry, validity and limits

A nonzero velocity produces one derived shaft and two arrow-head lines. The endpoint is `actor_position + captured_world_velocity * velocity_scale`; the arrow head is a display convention, 15% of the displayed vector length with 7.5% half width. This arrow shows a recorded velocity magnitude/axis, **not a predicted future trajectory**, integrated angular orientation or solver force. Angular vectors use the same explicit numeric display scale; their world-space arrow length is a visualization choice, not a physical angular displacement. Bounds, linear and angular layers have distinct viewer palette colors.

An exactly zero velocity draws nothing. A vector whose entire arrow vanishes at the current world-space float resolution is also omitted rather than enlarged. Scale zero disables only the velocity layers; bounds remain independently available. Negative/nonfinite scales and a zero diagnostic retention limit are invalid options and throw `invalid_argument`.

Captured scalar counts, byte lengths, byte order, typed accessors and finite components are checked. An unavailable/wrongly typed velocity or pose produces a diagnostic. Available data from another attachment/actor, a last frame, shape world pose or an estimated delta is never substituted. Double-precision intermediate math prevents unnecessary overflow; all final line endpoints must be finite representable floats. An overflowing arrow is rejected as a whole. Bounds validate all minima/maxima before emitting any edge. Sources remain unchanged.

All layers share the default 262,144 line budget. Bounds require 12 lines atomically and arrows 3; if the next complete primitive cannot fit, generation stops with an explicit incomplete result. No partial AABB or bare arrow shaft is committed. The deterministic order is Frame attachment order, its bounds, then its actor's first linear and angular vector. At most 262,144 input attachment rows are examined, including hidden rows, and a visibility mask is bounded by the same work limit. Diagnostics retain at most 256 entries, while `complete` remains false for later errors even when no more diagnostic strings are retained. Missing/invalid data can leave independent valid layers available; budget exhaustion preserves the already completed prefix.

## Verification

`tests/body_overlays_tests.cpp` checks little/big endian typed properties with 32/64 source pointer metadata, exact high-bit 64-bit picking IDs, source prefix/generation protection, per-attachment 12-edge tight boxes, actor-anchored world vectors, arrow parts/colors, multiple shapes/shared shapes, articulation links, static and zero vectors, explicit empty/partial visibility masks, hidden invalid fields, source immutability, missing/wrongly typed/truncated/nonfinite properties, invalid poses/bounds, finite-input endpoint overflow and whole-primitive line/work/diagnostic limits.

An optional argument loads an existing real SDK `physical_capture.pxd2` and extracts actual render Frames and snapshots. The current fixture validates 48 native frames, 58 tight dynamic-attachment boxes and 70 captured velocity vectors, comparing arrow anchors/endpoints against the actual typed Actor properties. No additional producer or SDK runtime is needed for this consumer check. Independent Clang and MSVC builds/logs are under `analysis/body-overlays`.

The ImGui View popup exposes all three layers and their numeric scale. The GPU renderer calls the helper only for enabled layers and passes the current visible attachment set, including an explicitly empty set when everything is hidden. Requested missing fields leave the independent valid layers visible and expose bounded source diagnostics in the popup and a viewport notice. `display_settings` also renders the actual 12-edge box and two 3-line arrows through D3D11 WARP, checks changed pixels, hides the last attachment and verifies the original image returns, and verifies a missing angular field is reported without dropping the available linear arrow.
