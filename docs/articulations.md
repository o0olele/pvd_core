# Native PhysX 3.4 articulations and aggregates

`examples/physx34_articulation_capture.cpp` is a real SDK producer, separate from
the independent core/render libraries. It exercises the existing extractor's
native `PxArticulationLink` path and the captured object graph. It does not
replace links or aggregates with fabricated rigid actors, scenes or shapes.

Given a prefix, it writes:

- `_capture.pxd2`: native `eDEBUG` recording with two actual SDK scenes.
- `_expected.csv`: independent SDK getters before and after each scene step.
- `_official.repx`: SDK-authored XML at the `articulation_oracle` section.

The SDK-only target uses the existing
`pvd_add_physx34_capture(target, source)` helper, overrides
`MSVC_RUNTIME_LIBRARY` to `MultiThreadedDebug`, and links
`PhysX3ExtensionsDEBUG.lib`. This matches the local Debug x64 Extensions
archive. It neither builds nor modifies the SDK. The test executable uses only
`pvd::core` and `pvd::render`, without SDK, UI or GPU dependencies:

```text
pvd_physx34_articulation_capture PREFIX
pvd_physx34_articulation PREFIX_capture.pxd2 PREFIX_expected.csv
```

## Workload and lifecycle

The initial workload contains a four-link branched articulation (root with two
children, plus a grandchild), a separate two-link articulation, three ordinary
rigid actors, nine exclusive box shapes and three aggregates. Both
`SelfCollision=false` and `true` aggregates contain native rigid actors and/or
articulations. Shared material, nonidentity actor and shape poses, nonzero
center-of-mass poses, masses/inertia and nondefault articulation/joint settings
are recorded. The second scene is independent of the first.

Ten steps remove/reinsert a rigid aggregate member, transfer it to another
aggregate, force sleep/wake, transfer an articulation between aggregates,
release/recreate its entire branch graph with the final link under a different
parent, move a **populated** aggregate across scenes, and release/recreate an
empty aggregate. Scene 0 consequently becomes empty of shapes while scene 1
contains all nine. Existing SDK links have no public in-place parent setter;
the topology change is a legitimate release/recreation, not a made-up reparent
event. Actual reused object IDs are distinguished by core generations.

The oracle records every articulation setting including solver iterations,
projection/separation, internal/external drive iterations, sleep thresholds and
wake counter. Each inbound joint records parent/child poses, target quaternion
and velocity, drive type, stiffness/damping, internal/external compliance,
swing/twist limits, contact distances and enabled flags. Link/rigid properties,
shape local poses, flags, materials and dimensions are independently queried.
World poses use the SDK's actor × shape transform; box world bounds use
`PxGeometryQuery::getWorldBounds(..., inflation=1)`.

## Actual native ownership, not assumed fields

The source binding explicitly sets root-link `Parent` to its articulation, and
child-link `Parent` to its parent link. `PxArticulation.Links` contains the root;
each link's `Links` contains its children. Walking this graph must equal the
complete SDK `getLinks()` set. A nonroot link has `InboundJoint`, whose `Link`
points back to that actual child. A root's null inbound joint may be absent.

Although class metadata inherits a `Scene` declaration, this SDK recording
does not send a direct `Scene` property on links. The extractor correctly
resolves their owner scene through `Parent`. It also does not send an
`Aggregate` property on actors or articulations. Aggregate ownership is
represented by the aggregate's `Actors` and `Articulations` collections.
`Actors` contains ordinary actors; `Articulations` contains each articulation
owner once. The aggregate's complete SDK actor count includes **all links**,
not just the number of articulation owners. These missing direct fields are
explicitly tested as unrecorded rather than interpreted as captured null IDs.

The real SDK emits duplicate `Shapes` references for some nonroot links even
though `getNbShapes()==1` and the producer attaches the shape once. The core
retains the original wire events/references. Rendering canonicalizes identical
actor/shape attachments as a set, preserving actual source IDs and generations.
The test verifies both the duplicated raw relation and one rendered attachment.

Aggregate member commands can be queued until that scene's next frame start.
An initial custom section therefore does not imply that these collections are
already complete. The test explicitly permits only the expected subset during
that narrow creation interval, and requires full exact membership after the
scene has begun a frame. The same interval is exercised when a populated
aggregate is reinserted in another scene.

## Native frame and getter timing

The CSV has eight columns:

```text
step,phase,object,class,property,component,type,value
```

Types are unsigned integer `u`, float `f`, reference `r`, string `s`, and unique
collection count `c`. Quaternion/transform components use the SDK layout
`qx,qy,qz,qw,px,py,pz`. Flattened names match actual PVD metadata, including
`SolverIterationCounts.minPositionIters` / `minVelocityIters` and
`SwingLimit.zLimit` / `yLimit`. The generated swing names are misleading: the
implementation's first getter output is y and the second z; the oracle preserves
the actual generated-name mapping rather than swapping values by intuition.
Properties starting with `@` are explicitly SDK/derived oracle fields, not
claims that such PVD properties exist. `@AllLinks` / `@AllActors` retain complete
public getter sets; shape `@WorldPose`, `@BoundsMin` / `@BoundsMax` and `@Scene`
are independent rendering expectations.

`initial` precedes simulation. Each step sequentially records `frame0`, simulates
and fetches scene 0, records `post0` and an explicit `validation_after_fetch`
section, then does `frame1` / `post1` for scene 1. Both scenes must therefore be
compared at their own event prefixes rather than guessed global frame offsets.
At step 1 after both fetches, `articulation_oracle` and the official XML describe
the same SDK state; this is the serialization oracle, not the initial section.

Native end-frame precedes link/rigid pose and velocity updates. Those properties
normally retain the preceding fetched sample; creation/reinsertion supplies the
new lifetime's initial values. Rigid dynamic sleep belongs to that update block.
Articulation owner simple properties, including wake counter and sleep, can
already be synchronized before native frame end. Link display sleep derives
from that actual owner, not an invented per-link sleep field. Initial creation
can retain free-standing `IsSleeping=true` before insertion wakes the SDK object.
The test models these source-proven update boundaries using independent SDK
getter history; it does not compare all fields to a single shifted timestep or
silently skip mismatches.

## Validation and scope

`tests/physx34_articulation.cpp` verifies exact live native object sets, complete
root/child and aggregate membership, scalar/reference fields, source lifetimes,
derived sleep/actor kind, nine actual shape attachments, dimensions, world poses
and independent bounds. Scene-filtered extraction must match the SDK scene of
every shape, including the populated cross-scene transfer and an empty scene.
It repeats all 41 samples forward and backward, then repeats with checkpoints
disabled. The local run compared 53,300 fields and 738 attachments per pass,
observed 17 reused native lifetimes, and passed MSVC `/W4` and Clang warning
checks without modifying the existing core/extractor.

This fixture validates native capture/replay/rendering and provides official XML
evidence for the separate RepX exporter. It does not by itself claim complete
RepX restoration, a new in-place topology-edit API, APEX support, or compatibility
with older SDK/container versions. See `docs/repx.md` for serializer-specific
support and native XML limitations.
