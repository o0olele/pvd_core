#pragma once
#include "pvd/render.hpp"
#include <array>
#include <limits>

namespace pvd::render {
enum class SceneQueryKind { raycast, sweep, overlap };
enum class SceneQueryCollectionState { uncollected, recorded, malformed };
enum class SceneQueryHitRole { unreported, blocking, touching };
struct SceneQuerySource {
  ObjectKey scene;
  SceneQueryKind kind = SceneQueryKind::raycast;
  bool batched = false;
  // Sequence within this kind's published array, not global call order.
  std::size_t sequence = 0, snapshot_event_count = 0,
              publication_event_count = 0;
  std::uint64_t stream_id = 0, outer_timestamp = 0, byte_offset = 0;
};
struct SceneQueryArrayRange {
  std::string array_name;
  std::uint32_t base_index = 0, count = 0;
};
struct SceneQueryHit {
  // Original source-ABI IDs and values remain available even when unresolved.
  std::uint64_t shape_id = 0, actor_id = 0;
  std::optional<ObjectKey> shape, actor;
  std::uint32_t face_index = 0, flags = 0;
  Vec3 position, normal;
  float distance = 0, u = 0, v = 0;
  bool position_available = false, normal_available = false,
       distance_available = false, uv_available = false,
       face_index_available = false;
  SceneQueryHitRole role = SceneQueryHitRole::unreported;
};
using SceneQueryFilterData = std::array<std::uint32_t, 4>;
struct SceneQueryConvex {
  std::uint64_t resource_id = 0;
  std::optional<ObjectKey> resource;
  MeshScale scale;
  std::uint32_t flags = 0;
};
struct SceneQueryGeometry {
  ObjectKey key;
  QualifiedName class_name;
  std::variant<Box, Sphere, Capsule, SceneQueryConvex> value;
};
struct SceneQuery {
  SceneQuerySource source;
  std::uint32_t type = 0, filter_flags = 0;
  // PhysX 3.4 collector explicitly does not transmit these input/return facts.
  std::optional<std::uint32_t> requested_hit_flags;
  std::optional<float> sweep_inflation;
  std::optional<bool> has_block;
  std::optional<Vec3> origin, unit_direction;
  std::optional<float> max_distance;
  std::optional<Transform> overlap_pose;
  SceneQueryArrayRange hits_range, geometry_range, pose_range,
      filter_data_range;
  std::vector<SceneQueryFilterData> filter_data;
  std::vector<Transform> poses;
  std::vector<SceneQueryGeometry> geometries;
  std::vector<SceneQueryHit> hits;
  // Structural validity only. Recorded empty hits do not prove a successful
  // miss: batch queryStatus/hasBlock and exhaustive collection are not sent.
  bool complete = true;
  // The SDK collects calls until fetchResults, without per-call origin epochs.
  // A shift since the prior publication can mix old/new coordinate systems.
  // Raw parameters/hits remain exact, but derived spatial previews are unsafe.
  bool coordinates_ambiguous = false;
};
struct SceneQueryCollection {
  SceneQueryKind kind = SceneQueryKind::raycast;
  bool batched = false;
  SceneQueryCollectionState state = SceneQueryCollectionState::uncollected;
  std::size_t publication_event_count = 0, query_count = 0;
};
enum class SceneQueryCode {
  missing_scene,
  invalid_range,
  invalid_type,
  malformed,
  missing_array,
  out_of_range,
  dangling_reference,
  unsupported_query,
  unsupported_geometry,
  stale_array,
  multiple_publications,
  limit_exceeded,
  ambiguous_coordinates
};
struct SceneQueryDiagnostic {
  SceneQueryCode code = SceneQueryCode::malformed;
  SceneQuerySource source;
  std::string property, message;
  std::size_t payload_offset = 0;
};
enum class SceneQueryLineKind { requested_path, impact_normal };
struct SceneQueryLine {
  SceneQuerySource source;
  SceneQueryLineKind kind = SceneQueryLineKind::requested_path;
  std::optional<std::size_t> hit_index;
  Vec3 position0, position1;
  std::uint32_t color = 0;
  bool derived = true;
};
struct SceneQueryWindow {
  std::uint64_t scene_id = 0;
  std::size_t begin_event_count = 0, end_event_count = 0;
};
struct SceneQueryOptions {
  std::size_t max_queries = 65536, max_hits = 262144, max_aux_items = 262144;
  std::size_t max_array_bytes = 64u * 1024u * 1024u,
              max_reference_string_bytes = 256;
  std::size_t max_lines = 262144, max_diagnostics = 256;
  std::size_t max_index_entries = 1024u * 1024u;
  bool emit_paths = true, emit_normals = true;
  float normal_length = .5f;
};
struct SceneQueries {
  ObjectKey scene;
  std::size_t begin_event_count = 0, event_count = 0;
  // Raycast/sweep/overlap, then the same three batched families.
  std::array<SceneQueryCollection, 6> collections{
      {{SceneQueryKind::raycast, false},
       {SceneQueryKind::sweep, false},
       {SceneQueryKind::overlap, false},
       {SceneQueryKind::raycast, true},
       {SceneQueryKind::sweep, true},
       {SceneQueryKind::overlap, true}}};
  std::vector<SceneQuery> queries;
  std::vector<SceneQueryLine> lines;
  std::vector<SceneQueryDiagnostic> diagnostics;
  bool complete = true;
};
// Incremental, UI-independent publication index. Session remains externally
// owned and must not append concurrently. reset before binding a new Session.
// Extraction uses only fresh publications inside the explicit window and its
// ending snapshot; old cached arrays are never repeated in an uncollected
// frame.
class SceneQueryIndex {
public:
  PVD_RENDER_API explicit SceneQueryIndex(SceneQueryOptions options = {});
  PVD_RENDER_API ~SceneQueryIndex();
  PVD_RENDER_API SceneQueryIndex(SceneQueryIndex &&) noexcept;
  PVD_RENDER_API SceneQueryIndex &operator=(SceneQueryIndex &&) noexcept;
  SceneQueryIndex(const SceneQueryIndex &) = delete;
  SceneQueryIndex &operator=(const SceneQueryIndex &) = delete;
  PVD_RENDER_API void reset();
  PVD_RENDER_API void
  update(const Session &,
         std::size_t event_count = std::numeric_limits<std::size_t>::max());
  PVD_RENDER_API std::size_t indexed_event_count() const noexcept;
  PVD_RENDER_API SceneQueries extract(const Session &, SceneQueryWindow);
  PVD_RENDER_API SceneQueries extract(const Session &, const FrameMarker &);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
PVD_RENDER_API SceneQueries extract_scene_queries(
    const Session &, SceneQueryWindow, SceneQueryOptions options = {});
PVD_RENDER_API SceneQueries extract_scene_queries(
    const Session &, const FrameMarker &, SceneQueryOptions options = {});
PVD_RENDER_API const char *scene_query_array_name(SceneQueryKind,
                                                  bool batched) noexcept;
} // namespace pvd::render
