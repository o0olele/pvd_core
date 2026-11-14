# PhysX 3.4 RepX export

`pvd::repx` exports a recorded rigid-object snapshot as PhysX RepX XML. Its
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
Only their transitive shape, material, and mesh/heightfield dependencies are
included. Actor/shape selection roots are supported; arbitrary unknown objects
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

## Strict and partial exports

`UnsupportedPolicy::reject` is the default. Unsupported actor classes,
articulations, joints/constraints, aggregates, cloth, particles/APEX actors,
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
