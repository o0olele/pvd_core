# Native PhysX 3.4 cloth and particles

`pvd/deformables.hpp` is part of `pvd::render`. It depends on the standard
library and the independent recording/session model. It does not link PhysX,
ImGui, a GPU API, or APEX. It returns native deformable owners and their sampled
elements; it does not invent rigid actors or `PxShape` objects.

```cpp
auto snapshot = session.snapshot_at_frame(frame_index);
pvd::deformables::Options options;
auto data = pvd::deformables::extract(session.metadata(), snapshot, options);
```

`Frame.event_count` identifies the immutable source sample. `OwnerKey` contains
scene and owner IDs **and generations**. Fabric references have their own
generation. `SelectionKey` distinguishes owner, cloth vertex, cloth constraint,
and particle selections. Native particle indices can be released and reused
while their owner stays alive. Since the stream has no per-particle generation
event, element selections include `sample_event_count`; clients must not claim
that a reused index is the same persistent particle.

Empty `Options.owners` means all supported owners, optionally filtered by
`Options.scene`. Explicit selections must identify a live object of the right
class and generation. Stale selections produce diagnostics. Filtering an owner
out also excludes that owner's malformed properties from the result.

## Cloth: current particles and exact solver edges

`ClothSurface` retains local current positions, inverse masses, fixed-particle
indices (`inverse_mass == 0`), `GlobalPose`, flags, sleeping state, fabric
resource, constraint pairs, rest values, set membership, and phase type/set.
World positions are computed as `GlobalPose.transform(local_position)`; bounds
enclose those world points. There is no identity-pose fallback when the pose
was not recorded. Local positions can remain available while world positions
are unavailable.

Native `ParticleBuffer` entries are `PxClothParticle`: three 32-bit floats for
local position and one 32-bit inverse mass. Fabric `ParticleIndices` are pairs
of `PvdU32` particle indices. `Restvalues` has one float per pair; `Sets` stores
monotonic cumulative constraint counts. `Phases` consists of 8-byte
`PxClothFabricPhase` records, phase type then set index. The supported native
phase types are vertical, horizontal, bending, and shearing (1–4).

**The native 3.4 PVD fabric metadata does not record the input mesh's triangles
or quads.** It also does not emit the public fabric tether arrays. Distance,
bending, and shear edges do not determine a unique surface triangulation.
Consequently `triangle_topology_status` is `not_recorded` and
`triangle_indices` is empty. The accurate display is current particles plus
recorded constraint edges. A client needing filled cloth faces must obtain the
original topology through an explicitly separate source. The extractor never
labels solver edges or a guessed triangulation as recorded surface topology.

Initial cloth creation does not necessarily send `ParticleBuffer`; the first
native cloth update supplies it. Such an initial snapshot retains the fabric
but reports missing particle data. Sleeping cloth may reuse its last captured
positions because the native sender skips unchanged sleeping states. Names,
pickability and color are retained from the owner.

## Particles: sparse indices and packed properties

Both `PxParticleSystem` and `PxParticleFluid` produce `ParticleCloud` objects.
`NbParticles`, `ValidParticleRange`, and `ValidParticleBitmap` reconstruct
ascending original sparse indices. The SDK packs property rows against those
set bits: row 1 is the second valid particle, not particle index 1. The
extractor maps positions, velocities, flags, per-particle rest offsets, fluid
densities, and valid collision normals back to each original index.

Particle-system positions and velocities are already in world coordinates;
they are not multiplied by a rigid/cloth pose. No extra accumulated origin
shift is added to already recorded coordinates. A missing position stays
`nullopt`, not a fabricated point at the origin. Bounds include only validated,
captured positions.

The sender writes `(ValidParticleRange >> 5) + 1` bitmap words, including an
extra padding word when the range is divisible by 32. The extractor accepts
that native form and the equivalent minimal word count. Set bits beyond the
valid range are ignored with a warning, never made into particles. The number
of in-range bits must equal `NbParticles`. Nonempty counts/ranges, declared
capacity, packed row counts, and types must agree.

The native sender intentionally does not clear old arrays when count and
range become zero. Therefore **zero count/range overrides all stale bitmap and
property data**, yielding an empty cloud without stale bounds. An inconsistent
zero count with a nonzero range is rejected. `ParticleBaseFlags::eENABLED` is
bit 2 (`4`); when it is clear native frame updates are skipped.
`updates_disabled` and a diagnostic identify the possibly stale recorded state.

The initial public-read-data path honors read flags. The CPU frame path uses
internal state, so it can later send positions, velocities, and flags even
when the application's corresponding public read flag is off. Captured valid
arrays take precedence over that flag; absent disabled arrays report
`DataStatus::disabled`. Absent enabled arrays report `not_recorded`. Optional
density and collision data also depend on native simulation/readback state.
Collision normals are meaningful only when the captured particle flags carry
a collision/drain bit; undefined noncollision entries are not read or exposed.
The SDK does not send a collision-velocity array in this PVD path.

## Diagnostics and limits

Each relevant buffer has explicit availability. Malformed buffers produce
`DataStatus::malformed`, bounded diagnostics, and no partially decoded buffer.
A bad fabric does not discard otherwise valid cloth particle positions.
Missing optional data is reported rather than guessed. `Frame.complete` is
false on warnings/errors or suppressed diagnostics; informational limitations
(such as absent original cloth faces) do not mean the captured bytes are
corrupt. Callers must still inspect availability and diagnostics rather than
using `complete` as a promise that every feature was captured.

`Options` bounds owners, total cloth vertices/constraints/particles, sparse
index range, bitmap words, bytes per property, and diagnostics. Limits must be
positive. Incoming scalar and array types, exact byte/item counts, references,
indices, finite values, nonnegative masses/densities/rest offsets, unit pose
quaternions, and phase/set bounds are validated. Endianness and source pointer
width come from each source property, not from the host ABI.

APEX cloth/particles, GPU-exclusive buffers that were never streamed, original
cloth faces/tethers, particle IDs across slot reuse, and unrecorded properties
cannot be reconstructed by this module. They are not silently converted into
other geometry. A recording with none of the three native owner classes yields
empty owner lists; the module cannot infer whether an application simulated
an unrecorded external object.

## SDK evidence and reproducible validation

The implementation was checked against the local PhysX 3.4 source paths:

- `Source/PhysX/src/PvdMetaDataPvdBinding.cpp`: native owner/fabric metadata,
  `ParticleFluidUpdater`, `sendArrays`, `sendAllProperties(PxClothFabric)`, and
  `updateCloths`.
- `Source/PhysX/src/buffering/ScbScenePvdClient.cpp`: `sendStateDatas` and
  `frameEnd`; particle and cloth updates occur **before** native frame end.
- `Source/PhysXMetaData/core/include/PxAutoGeneratedMetaDataObjects.h`:
  actual fabric fields and typed particle/cloth properties.
- `Source/LowLevelParticles/src/PtParticleData.cpp` and
  `PtParticleSystemSimCpu.cpp`: CPU state/readback availability.
- `Include/cloth/PxClothTypes.h`, `PxClothFabric.h`, `PxCloth.h`, and
  `Include/particles/PxParticleReadData.h`, `PxParticleBaseFlag.h`: native
  layouts, local/world coordinate contracts, flags, and validity semantics.

`examples/physx34_deformables_capture.cpp` is an optional, separate SDK oracle.
It uses `eDEBUG`, a file transport, CPU cloth/fabric cooking, and four native
particle owners. Its SDK-only executable uses `/MTd` with the supplied
`PhysX3ExtensionsDEBUG.lib`; the independent core/render libraries do not link
that archive. The SDK tree remains read-only. Given a prefix it writes
`<prefix>_capture.pxd2` and `<prefix>_expected.csv`.

The fixture has initial state plus eight simulation steps, sparse particle
indices crossing 31/32/63, release/recreate and empty transitions, a public
velocity-read-off owner and independent SDK twin, fluid density, disabled
updates, fluid owner reuse, a nonidentity moving/rotating cloth pose, fixed
corner and edited pin weights, sleep/wake, cloth owner reuse, and a reused fabric.
Expected positions are read from SDK particle locks. Cloth world positions
are independently computed with the SDK's `PxTransform`. Fabric arrays are
copied through SDK public accessors. Named validation sections after fetch
avoid assuming a frame offset; the tests also compare native frame snapshots
directly and repeat them in reverse.

`pvd_deformables_tests` runs synthetic little/big endian × 32/64-bit source
tests without the SDK. Passing the recording and CSV as its two arguments adds
the real SDK comparison. The tests cover availability, sparse expansion,
world transforms, fixed points, exact constraints/phases, zero/stale arrays,
lifetimes/generations, invalid types/counts/references/indices, nonfinite and
negative data, malformed payloads, selection filtering, and resource limits.
