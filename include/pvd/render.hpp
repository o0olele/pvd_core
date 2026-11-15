#pragma once
#include "pvd/render_export.hpp"
#include "pvd/session.hpp"
#include <memory>
#include <variant>

namespace pvd::render {

struct ObjectKey {
  std::uint64_t id = 0, generation = 0;
  bool operator==(const ObjectKey &o) const {
    return id == o.id && generation == o.generation;
  }
  bool operator<(const ObjectKey &o) const {
    return std::tie(id, generation) < std::tie(o.id, o.generation);
  }
};
struct PickingKey {
  ObjectKey scene, actor, shape;
  bool operator==(const PickingKey &o) const {
    return scene == o.scene && actor == o.actor && shape == o.shape;
  }
  bool operator<(const PickingKey &o) const {
    return std::tie(scene, actor, shape) < std::tie(o.scene, o.actor, o.shape);
  }
};
enum class GeometryKind {
  box,
  sphere,
  capsule,
  plane,
  convex_mesh,
  triangle_mesh,
  height_field
};
struct Bounds {
  Vec3 minimum, maximum;
};
struct Box {
  Vec3 half_extents;
};
struct Sphere {
  float radius = 0;
};
struct Capsule {
  float radius = 0, half_height = 0;
};               // Segment is on local X.
struct Plane {}; // Infinite local plane X=0; no finite world_bounds.
struct MeshScale {
  Vec3 scale{1, 1, 1};
  Quat rotation;
  // SDK matrix is R^T diag(scale) R, never diag(scale) R.
  bool flips_winding() const {
    return (double(scale.x) * scale.y * scale.z) < 0;
  }
};
struct MeshData {
  ObjectKey resource;
  GeometryKind kind = GeometryKind::triangle_mesh;
  // Unscaled resource-local coordinates. HF coordinates are (row,signed
  // height,column).
  std::vector<Vec3> vertices;
  // Native winding; consumers flip triangle winding for negative MeshScale
  // determinant.
  std::vector<std::uint32_t> indices;
  // Optional per emitted triangle, including HF hole removal; empty means
  // unavailable.
  std::vector<std::uint16_t> material_indices;
};
struct ConvexMesh {
  std::shared_ptr<const MeshData> mesh;
  MeshScale scale;
  std::uint32_t mesh_flags = 0;
};
struct TriangleMesh {
  std::shared_ptr<const MeshData> mesh;
  MeshScale scale;
  std::uint32_t mesh_flags = 0;
  bool double_sided = false;
};
struct HeightField {
  std::shared_ptr<const MeshData> mesh;
  std::uint32_t rows = 0, columns = 0;
  float height_scale = 0, row_scale = 0, column_scale = 0;
  std::uint32_t flags = 0;
  bool double_sided = false;
};
using Geometry = std::variant<Box, Sphere, Capsule, Plane, ConvexMesh,
                              TriangleMesh, HeightField>;
inline GeometryKind geometry_kind(const Geometry &g) {
  return static_cast<GeometryKind>(g.index());
}
enum class ActorKind { rigid_static, rigid_dynamic, articulation_link };
struct ActorStatus {
  ActorKind kind = ActorKind::rigid_static;
  bool kinematic = false;
  std::optional<bool> sleeping;
  std::uint32_t rigid_body_flags = 0;
};
struct ShapeFlags {
  std::uint32_t raw = 0;
  bool simulation = false, scene_query = false, trigger = false,
       visualization = false;
};
struct ShapeInstance {
  std::uint64_t actor_id = 0, actor_generation = 0, shape_id = 0,
                shape_generation = 0;
  std::uint64_t scene_id = 0, scene_generation = 0, geometry_id = 0,
                geometry_generation = 0;
  Transform world_pose;
  Geometry geometry;
  ShapeFlags flags;
  ActorStatus actor;
  std::optional<Color> color;
  bool pickable = true, selected = false;
  // Tight analytic/vertex bounds, independent of SDK broadphase
  // inflation/padding.
  std::optional<Bounds> world_bounds;
  PickingKey picking_key() const {
    return {{scene_id, scene_generation},
            {actor_id, actor_generation},
            {shape_id, shape_generation}};
  }
};
enum class Severity { warning, error };
enum class DiagnosticCode {
  missing_property,
  invalid_type,
  malformed_payload,
  dangling_reference,
  unsupported_geometry,
  invalid_value,
  invalid_index,
  count_mismatch,
  resource_limit,
  missing_scene,
  unsupported_actor
};
struct Diagnostic {
  Severity severity = Severity::error;
  DiagnosticCode code = DiagnosticCode::invalid_value;
  std::uint64_t object_id = 0, actor_id = 0, shape_id = 0;
  std::string property, message;
};
struct ExtractOptions {
  std::optional<std::uint64_t> scene_id;
  std::vector<PickingKey> selected;
};
struct ExtractorOptions {
  std::size_t max_vertices = 4u * 1024u * 1024u;
  std::size_t max_triangles = 8u * 1024u * 1024u;
  std::size_t max_heightfield_cells = 4u * 1024u * 1024u;
  std::size_t max_cache_entries = 128;
};
struct CacheStats {
  std::size_t entries = 0, hits = 0, misses = 0;
};
struct Frame {
  std::size_t event_count = 0;
  std::vector<ShapeInstance> instances;
  std::vector<Diagnostic> diagnostics;
  // Explicit capture offsets only. Poses already use current native SDK
  // coordinates; this extractor does not apply origin shifts a second time or
  // change handedness.
  std::map<std::uint64_t, Vec3> origin_shifts;
};
struct GeometryExtraction {
  ObjectKey source;
  std::optional<Geometry> geometry;
  std::vector<Diagnostic> diagnostics;
};
PVD_RENDER_API Vec3 rotate(const Quat &rotation, const Vec3 &point);
PVD_RENDER_API Vec3 transform_point(const Transform &pose, const Vec3 &point);
PVD_RENDER_API Transform compose(const Transform &parent,
                                 const Transform &local);
PVD_RENDER_API Vec3 apply_mesh_scale(const MeshScale &scale, const Vec3 &point);

// Mutable geometry cache; externally synchronize calls on one Extractor. Frames
// share immutable meshes and remain valid after eviction/clear or later
// extract.
class Extractor {
public:
  PVD_RENDER_API explicit Extractor(ExtractorOptions options = {});
  PVD_RENDER_API ~Extractor();
  PVD_RENDER_API Extractor(Extractor &&) noexcept;
  PVD_RENDER_API Extractor &operator=(Extractor &&) noexcept;
  Extractor(const Extractor &) = delete;
  Extractor &operator=(const Extractor &) = delete;
  PVD_RENDER_API Frame extract(const Metadata &, const Snapshot &,
                               ExtractOptions options = {});
  PVD_RENDER_API Frame extract(const Metadata &, const SnapshotView &,
                               ExtractOptions options = {});
  // Decode an actual recorded geometry object without actor/shape ownership.
  // Useful for scene-query volumes. Reuses this extractor's validated resource
  // cache and budgets; stale generations fail without inventing scene objects.
  PVD_RENDER_API GeometryExtraction extract_geometry(const Metadata &,
      const Snapshot &, ObjectKey);
  PVD_RENDER_API GeometryExtraction extract_geometry(const Metadata &,
      const SnapshotView &, ObjectKey);
  PVD_RENDER_API void clear_cache() noexcept;
  PVD_RENDER_API CacheStats cache_stats() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::render
