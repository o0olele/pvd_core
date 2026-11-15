#pragma once
#include "pvd/render_export.hpp"
#include "pvd/session.hpp"

namespace pvd::deformables {
struct ObjectKey {
  std::uint64_t id = 0, generation = 0;
  bool operator<(const ObjectKey &o) const {
    return std::tie(id, generation) < std::tie(o.id, o.generation);
  }
  bool operator==(const ObjectKey &o) const {
    return id == o.id && generation == o.generation;
  }
};
struct OwnerKey {
  ObjectKey scene, owner;
  bool operator<(const OwnerKey &o) const {
    return std::tie(scene, owner) < std::tie(o.scene, o.owner);
  }
  bool operator==(const OwnerKey &o) const {
    return scene == o.scene && owner == o.owner;
  }
};
enum class ElementKind { owner, cloth_vertex, cloth_constraint, particle };
struct SelectionKey {
  OwnerKey object;
  ElementKind kind = ElementKind::owner;
  std::uint32_t index = 0;
  // Native particle indices can be reused without an object-generation event.
  // Element selections are scoped to this immutable sample, not fabricated IDs.
  std::size_t sample_event_count = 0;
  bool operator<(const SelectionKey &o) const {
    return std::tie(object, kind, index, sample_event_count) <
           std::tie(o.object, o.kind, o.index, o.sample_event_count);
  }
  bool operator==(const SelectionKey &o) const {
    return object == o.object && kind == o.kind && index == o.index &&
           sample_event_count == o.sample_event_count;
  }
};
struct Bounds {
  Vec3 minimum, maximum;
};
enum class DataStatus { available, not_recorded, disabled, malformed };
struct ClothConstraint {
  std::uint32_t index = 0, particle0 = 0, particle1 = 0;
  std::optional<float> rest_length;
  std::optional<std::uint32_t> set_index;
};
struct ClothPhase {
  std::uint32_t phase_type = 0, set_index = 0;
};
struct ClothSurface {
  OwnerKey key;
  std::optional<ObjectKey> fabric;
  std::string name;
  bool pickable = true;
  std::optional<Color> color;
  std::optional<std::uint32_t> actor_flags, cloth_flags, declared_count;
  std::optional<bool> sleeping;
  std::optional<Transform> global_pose;
  DataStatus local_positions_status = DataStatus::not_recorded;
  // Availability of world_positions (requires a captured valid GlobalPose).
  DataStatus positions_status = DataStatus::not_recorded;
  // All arrays are indexed by the original PxCloth particle index.
  std::vector<Vec3> local_positions, world_positions;
  std::vector<float> inverse_masses;
  std::vector<std::uint32_t> pinned_indices;
  std::vector<ClothConstraint> constraints;
  std::vector<ClothPhase> phases;
  DataStatus constraints_status = DataStatus::not_recorded;
  // Native 3.4 fabric/PVD has no original mesh triangle/quad topology. Fabric
  // distance/bending/shear pairs must never be presented as surface triangles.
  DataStatus triangle_topology_status = DataStatus::not_recorded;
  std::vector<std::uint32_t> triangle_indices;
  std::optional<Bounds> world_bounds;
};
enum class ParticleKind { system, fluid };
struct Particle {
  std::uint32_t index = 0; // SDK sparse index, never compact-array index.
  std::optional<Vec3>
      position; // Native particle-system positions are world-space.
  std::optional<Vec3> velocity, collision_normal;
  std::optional<std::uint32_t> flags;
  std::optional<float> rest_offset, density;
};
struct ParticleCloud {
  OwnerKey key;
  ParticleKind kind = ParticleKind::system;
  std::string name;
  bool pickable = true;
  std::optional<Color> color;
  std::optional<std::uint32_t> actor_flags, base_flags, read_flags;
  bool updates_disabled =
      false; // eENABLED clear: arrays can be last captured state.
  std::optional<std::uint32_t> max_particles, declared_count, valid_range;
  DataStatus indices_status = DataStatus::not_recorded;
  DataStatus positions_status = DataStatus::not_recorded,
             velocities_status = DataStatus::not_recorded;
  DataStatus flags_status = DataStatus::not_recorded,
             densities_status = DataStatus::not_recorded;
  DataStatus rest_offsets_status = DataStatus::not_recorded,
             collision_normals_status = DataStatus::not_recorded;
  // Packed properties are expanded against ascending set bits of the validity
  // bitmap. Missing position data stays null; no origin points are fabricated.
  std::vector<Particle> particles;
  std::optional<Bounds> world_bounds;
};
enum class Severity { info, warning, error };
enum class DiagnosticCode {
  missing_property,
  invalid_type,
  malformed_payload,
  count_mismatch,
  invalid_value,
  invalid_index,
  dangling_reference,
  missing_scene,
  resource_limit,
  data_not_recorded,
  read_flag_disabled,
  topology_not_recorded,
  padding_bits
};
struct Diagnostic {
  Severity severity = Severity::error;
  DiagnosticCode code = DiagnosticCode::malformed_payload;
  ObjectKey owner;
  std::string property, message;
};
struct Options {
  std::optional<ObjectKey> scene;
  // Empty means all native cloth/particle owners in the selected scene.
  std::vector<ObjectKey> owners;
  std::size_t max_owners = 4096, max_cloth_vertices = 4u * 1024u * 1024u;
  std::size_t max_constraints = 8u * 1024u * 1024u,
              max_particles = 4u * 1024u * 1024u;
  std::size_t max_valid_range = 16u * 1024u * 1024u,
              max_bitmap_words = 1024u * 1024u;
  std::size_t max_property_bytes = 64u * 1024u * 1024u, max_diagnostics = 4096;
};
struct Frame {
  std::size_t event_count = 0;
  std::vector<ClothSurface> cloths;
  std::vector<ParticleCloud> particle_clouds;
  std::vector<Diagnostic> diagnostics;
  std::size_t suppressed_diagnostics = 0;
  bool complete = true;
};
PVD_RENDER_API Frame extract(const Metadata &, const Snapshot &,
                             const Options & = {});
// Reads immutable shared instances directly; does not materialize a Snapshot.
PVD_RENDER_API Frame extract(const Metadata &, const SnapshotView &,
                             const Options & = {});
} // namespace pvd::deformables
