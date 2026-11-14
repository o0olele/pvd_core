# PhysX 3.4 RepX export

`pvd::repx` exports a recorded rigid actor/joint snapshot as PhysX RepX XML. Its
public header is `pvd/repx.hpp`; it links `pvd::core` and privately uses the
SDK-independent render extractor. It has no PhysX, ImGui, GPU, or simulation
runtime dependency. The optional validation executable is a separate process
that uses a locally supplied SDK.

```cpp
pvd::repx::Options options;
options.selection = pvd::repx::Selection::objects;
options.objects = {{actor_id, actor_generation}};
auto result = pvd::repx::export_session(session, frame.event_count, options);
pvd::repx::save_file(utf8_path, result); // existing files are refused
```

`export_snapshot(metadata, snapshot, options)` is also available. The default
selection is all supported actors, optionally scoped by a live scene key.
Selected or visible export uses explicit immutable object keys supplied by the
caller. Keys include generation and stale keys are rejected. Selecting a shape
includes all its supported owning actors; this preserves shared-shape identity.
Their transitive shape, material, mesh/heightfield dependencies and eligible
extension joints are included. Joint endpoints never expand this actor selection. Actor/shape selection roots are supported; arbitrary unknown objects
are not quietly converted to rigid bodies.

For viewport selection or visibility, use `Selection::attachments` and
`Options.attachments`, whose `AttachmentKey` contains scene, actor and shape
keys with their generations. This mode exports only those exact attachments,
even for shared shapes. Selecting `shared_actor_a`'s attachment does not restore
the hidden `shared_actor_b` attachment, and hidden sibling shapes on the same
actor stay excluded. Filtering occurs before geometry extraction, resource
budgets and validation, so an excluded sibling does not reappear indirectly.
Attachment keys are checked against the scene and the actor's source Shapes
membership; duplicates are deduplicated and original shape order is retained.
The older `objects` shape-root behavior still expands all owners by design;
use that mode only when that expansion is intended.

This is a rigid-object collection, not a complete simulation checkpoint. RepX
does not serialize a `PxScene` with its solver, callbacks, broadphase, contacts,
debug rendering, profile/memory histories, or application state. Recorded poses
are already native PhysX coordinates. Explicit PVD origin shifts are not applied
a second time. `UpVector` is Y-up and the captured physics tolerance scale is
written when available; SDK documentation describes those XML parameters as
application metadata rather than changes to the physics instance.

## Supported content and source evidence

The target format is the local SDK's actual
`<PhysX30Collection version="3.4.2">` representation. Its name does not mean
PhysX 3.0 is required. The implementation was checked against these local SDK
sources under `PhysX_3.4/Source`:

- `PhysXExtensions/src/serialization/Xml/SnXmlSerialization.cpp` supplies the
  root, version, dependency ordering and exclusive-shape treatment.
- `SnRepXCoreSerializer.cpp` defines material, primitive, mesh, convex and
  heightfield construction, including uncooked resource fallback.
- `SnXmlVisitorReader.h` and `SnXmlVisitorWriter.h` define shape references,
  inline shapes and dynamic property behavior.
- `SnXmlSerializer.h` defines canonical packed heightfield sample integers.
- `PhysXMetaData/core` supplies generated field/enum names and the specialized
  kinematic wake-counter accessor.

The exporter covers box, sphere, capsule, plane, triangle mesh, convex mesh and
heightfield geometries, preserving actor and shape-local transforms. Capsules
use the SDK's local X axis. Mesh scale includes the full nonuniform scale and
its quaternion; it is not baked into resource vertices. Shared resources and
shared shapes are emitted once and referenced by their dependency owners.

Material friction/restitution, combine modes and material flags are restored.
Shape state includes material bindings, Name, local pose, contact/rest offsets,
filter words and simulation/query/trigger/visualization flags. Actor state
includes Name, global pose, actor/client flags, client/dominance indices and
shape bindings. Dynamic state includes center-of-mass pose, mass/inertia,
linear/angular velocity, body/lock flags, damping, limits, sleep/stabilization
thresholds, wake counter and solver iteration counts, subject to the native
kinematic limitations below.

Transforms use `qx qy qz qw px py pz`. All floating values use locale-independent
round-trip float precision. Integer IDs/filter words/flags never pass through
floating point. Generated collection IDs are dense nonzero integers; the
`Result.objects` mapping preserves original 64-bit source IDs and generations.

Exclusive shapes are nested inside their actor's `<Shapes><PxShape>…` element.
Shared shapes use a top-level `PxShape` and `PxShapeRef`. This distinction is
preserved. The SDK's inline reader assigns exclusive shapes no collection-level
serial ID, so their mapping has `serial_id=0`, plus `owner_serial_id` and
`shape_index`. These locators are used by the independent validator. Converting
every shape into a shared shape would lose observable ownership state.

Triangle resources use `PxBVH33TriangleMesh`, `Points`, `Triangles`, and optional
material indices. Convex resources use `PxConvexMesh` and lowercase `points`.
Heightfields use `NbRows`, `NbColumns`, `Format=eS16_TM`, thickness/edge/flags,
and lowercase `samples`. Each sample is a canonical unsigned integer with
height in the low 16 bits and both original material/tessellation bytes above
it; holes, tessellation and reserved sample bits are preserved. BE captures are
converted field by field, rather than treating a heightfield cell as one
endian-swapped 32-bit scalar.

Cooked bytecode and source cooking settings are not available in ordinary PVD
geometry records. The XML intentionally omits `CookedData`, and the receiving
SDK cooks the recorded points/triangles/samples. This path is supported by the
official serializer and has been exercised with actual SDK loading. The
triangle midphase is explicitly BVH33; original acceleration structures are not
preserved. Native convex fallback cooking defaults to `vertexLimit=255`;
larger hulls are refused because exact reconstruction cannot be guaranteed.
Missing per-triangle material indices with multiple bound materials are also
refused. A single-material shape has no ambiguous material choice.

## Six extension joint types

The exporter supports the PhysX 3.4 `PxFixedJoint`, `PxDistanceJoint`,
`PxPrismaticJoint`, `PxRevoluteJoint`, `PxSphericalJoint` and `PxD6Joint` XML
serializers. The local SDK sources `serialization/Xml/SnJointRepXSerializer.cpp`,
`PhysXMetaData/extensions` generated metadata and `ExtPvd.cpp` define the
construction and writable property schema. Runtime-derived relative transforms,
velocities, angles, source scene pointers, constraint pointers and application
user-data are not serialized as independent physics properties.

Common fields preserve actor/world references, both local anchor poses, Name,
break force/torque, writable constraint flags and all four inverse mass/inertia
scales. Per-type fields preserve:

- Fixed: linear and angular projection tolerances.
- Distance: minimum/maximum distance, tolerance, spring stiffness/damping and
  distance flags, including their non-contiguous bit positions.
- Prismatic: the complete linear limit pair, enable flag and projection bounds.
- Revolute: the complete angular limit pair, drive velocity/force/gear ratio,
  drive/free-spin/limit flags and projection bounds.
- Spherical: complete cone limits, enable flag and linear projection tolerance.
- D6: all six axis motions, linear/twist/swing limits, all six drive spring,
  damping, force and acceleration flags, drive pose/linear/angular velocities,
  and projection tolerances.

Every limit includes restitution, bounce threshold, stiffness, damping and
contact distance. Finite values, SDK-valid ranges, transforms, motion enums and
known flag masks are checked before XML emission. PVD uses names such as
`LocalPose[eACTOR0]`, `Motion[eTWIST]`, `Drive[eX].ForceLimit`; XML uses nested
`LocalPose/eACTOR0`, `Motion/eTWIST`, `Drive/eX/ForceLimit`. Actor references are
remapped to collection IDs while zero remains the SDK's world attachment.

A joint is included when all non-world endpoints are selected and successfully
exported. A one-actor/world joint is valid; two world endpoints, identical
endpoints and pairs with no dynamic actor are rejected. A joint touching a
selected actor and an unselected actor is a selection-boundary error. Strict
mode fails. Explicit partial mode omits that joint with a source diagnostic and
XML warning, without adding the unselected actor. Unrelated joints elsewhere in
the scene do not make a selected/visible subcollection fail. Exact attachment
selection keeps its existing shape/owner boundary. Selecting a closed component
of five actors in the SDK fixture retains its four internal joints; selecting
one world-connected actor retains exactly that actor and its joint.

`eBROKEN` is read-only and `eGPU_COMPATIBLE` is internal: the native SDK
`NpConstraint::setFlags` rejects setting either. Such state is strictly rejected
or explicitly omitted under partial export. Custom shader constraints and
unknown joint subclasses remain unsupported. A recorded nondefault backing
constraint `MinResponseThreshold` is also rejected/omitted: the generic SDK
joint serializer does not restore it.

The SDK XML reader constructs an auxiliary `PxConstraint` for each extension
joint. These objects count against `max_objects`, but are not invented as PVD
objects with fabricated source generations. `Result.joint_count` counts exported
joints. `Result.objects` maps their real source IDs/generations to collection IDs;
the independent validator covers each automatic constraint through its joint.

Zero flags are written as the nonempty token `0`. The SDK 3.4 generic reader
skips an empty flags value instead of calling the setter, which could otherwise
retain a nonzero constructor default (including actor visualization or
constraint drive-force flags). The reader's flag-name lookup resolves `0` to
zero; actual SDK loading verifies all-zero actor/shape/material/joint flag
variants. Nonzero flags always use validated SDK names. This also fixes the
rigid-object zero-flags case rather than only the new joint fields.

## Strict and partial exports

`UnsupportedPolicy::reject` is the default. Unsupported actor classes,
articulations, custom joints/constraints, aggregates, cloth, particles/APEX actors,
unknown requested roots, stale references, invalid geometry/types/flags and
unavailable required properties are reported. Unknown top-level classes are
not declared a complete rigid export. Scene-scoped relationship checks consider
connections to the selected actors. Data/resource failures can stop preparation
even under partial mode; this mode is not permission to guess corrupt geometry.

`allow_partial` explicitly permits omission of unsupported requested content and
known native serialization losses. `Result.complete` becomes false, diagnostics
identify source objects/properties, and warning provenance is persisted as XML
comments. Consumers must show these warnings before presenting the artifact as
a successful partial export. Empty or wholly unsupported selections fail.

Actual SDK validation exposed a native format limitation: the RepX reader skips
kinematic linear/angular velocity, its specialized wake-counter setter skips
kinematic bodies, and it does not reconstruct a pending kinematic target on a
detached actor. These fields can be derived by running a scene, but cannot be
faithfully restored by this XML loader alone. A nonzero kinematic velocity/wake
counter or pending target therefore causes strict export to fail. Explicit
partial export retains the actor, shape and supported fields, reports that
loaded velocities/wake counter are zero and no pending target is restored, and
tests those actual SDK results. A clean parse is not advertised as exact
kinematic continuation. `IsSleeping` is scene-derived, not a standalone RepX
property; sleeping behavior after scene insertion depends on that scene.

UTF-8 names and XML entities are validated. Ampersands, angle brackets, quotes,
apostrophes, CR/LF/tab and boundary spaces are encoded so the SDK roundtrip
retains their bytes. Invalid UTF-8, forbidden XML control characters and
non-finite scalar values are refused. Object, vertex, triangle, heightfield-cell
and XML-byte budgets bound preparation/output. Cancellation is cooperative
during preparation and XML/file emission and throws `ExportCancelled`.

`save_file` accepts UTF-8 paths, exclusively creates a private sibling, writes
and flushes it, then publishes it atomically. Existing destinations are refused
unless `overwrite=true` was explicitly requested. Exceptions/cancellation keep
an existing destination untouched and remove only the private file created by
this invocation. The caller can also `write(ostream, result)`, but owns any
rollback for that stream. Crash durability of the directory entry is not
guaranteed; a crash may leave a `.pvd-tmp-*` sibling.

## Independent SDK validation

`examples/physx34_repx_validate.cpp` links the SDK in a separate `/MTd` process,
matching the supplied `PhysX3ExtensionsDEBUG.lib`. Its only boundary with the
`/MDd` application is XML/CSV files. No SDK is built or modified. Include
`cmake/PhysX34.cmake`, then `cmake/RepxValidation.cmake`, and call
`pvd_add_physx34_repx_validate(target)` for the optional validator.

`pvd_repx_tests` runs standalone unit checks. With the recorded seven-geometry
fixture and an output prefix it produces independent expected fields and XML:

```text
pvd_repx_tests render_capture.pxd2 exported
physx34_repx_validate exported.repx exported_expected.csv --oracle official.repx
```

The validator calls actual `PxSerialization::createCollectionFromXml`, locates
all top-level and inline objects, and compares loaded SDK getters with expected
snapshot values. The optional `--oracle` writes those objects using the official
`serializeCollectionToXml`, reloads that official XML and repeats the same
checks. Thus format compatibility is exercised through both native read and
write paths. Raw object IDs, flags, references, sample integers and Name bytes
are exact; floats use a `1e-5 * max(1, abs(expected))` tolerance. Mesh vertex/face
ordering is canonicalized because cooking may reorder it; triangle winding,
positions and normals remain checked.

The current real fixture validates 27 objects and 775 fields across all seven
geometries, including 10 exclusive shapes and one shared shape attached to two
actors. Equivalent BE source values produce identical XML and pass the same
775-field test. A strict nonuniform rotated mesh selection validates four
objects/148 fields; shared-shape selection validates both owners in four
objects/74 fields. UTF-8/entity/control-whitespace Name coverage validates three
objects/53 fields. The real full fixture intentionally uses explicit partial
mode for its moving kinematic capsule and checks the documented losses rather
than pretending those values survived. The selected and synthetic fixtures are
strict complete exports.

Enable `PVD_BUILD_REPX` to build/install the `pvd::repx` CMake target; it requires
`PVD_BUILD_RENDER`. Standalone tests pass in Debug static and Release shared
builds. Optional SDK CTest fixtures regenerate all six collections and validate
each with the SDK, including a second native serialize/reload pass.

The `_attachment` fixture exports only `shared_actor_a`'s shared-shape
attachment. Actual SDK reload must contain one actor and one still-shared
shape, with no hidden `shared_actor_b`. Unit checks also cover hidden invalid
sibling geometry, rebased exclusive-shape indices, duplicate keys, input shape
ordering, immutable source snapshots and stale scene/actor/shape generations.


### Joint source and official XML oracle

`examples/physx34_joints_capture.cpp` creates six real SDK joint types with
nondefault common anchors/scales/break values, limits, motors and D6 drives.
Two joints attach a body to the world; the other four connect a shared static
anchor to distinct dynamic bodies. A `joint_oracle` section after fetch bounds
the exact recorded snapshot. The producer writes the PVD capture, 243 direct SDK
getter components (`_joints.csv`) and SDK-authored `_official.repx` schema.

```text
pvd_repx_tests joints_capture.pxd2 joints_joints.csv exported_joints
physx34_repx_validate exported_joints.repx exported_joints_expected.csv --oracle official_cycle.repx
```

The test first compares all 243 SDK getter components with independently decoded
PVD snapshot values. It then exports nine variants: all, big endian, one
world-connected actor, partial boundary, a closed component, exact visible
attachments, partial broken joint, all-zero flags and UTF-8 joint names. The
expected loader fields use source snapshot values and the independent getter
property list, not the exporter's internal schema/DTO.

The full collection loads as 27 SDK objects: 21 mapped material/actor/shape/joint
objects plus six automatic constraints. The validator checks 776 getter fields,
including joint references, anchors, limits, drives and flags. Export -> native
SDK loader -> official XML writer -> native SDK loader repeats those checks.
One world actor/joint validates five objects and 111 fields; the selected/visible
closed component validates 19 objects and 559 fields. Boundary-partial exports
validate only the selected actor's three dependencies with no widened anchor.
Tests also cover read-only broken/internal flags, missing drive data, invalid
motion/cone/pose values, stale actor generations, source immutability and exact
object budgets including auxiliary constraints.
