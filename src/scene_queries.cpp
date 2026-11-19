#include "pvd/scene_queries.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <type_traits>

namespace pvd::render {
namespace {
const char *properties[] = {
    "SceneQueries.Raycasts",         "SceneQueries.Sweeps",
    "SceneQueries.Overlaps",         "SceneQueries.Hits",
    "SceneQueries.PoseList",         "SceneQueries.FilterDataList",
    "SceneQueries.GeometryList",     "BatchedQueries.Raycasts",
    "BatchedQueries.Sweeps",         "BatchedQueries.Overlaps",
    "BatchedQueries.Hits",           "BatchedQueries.PoseList",
    "BatchedQueries.FilterDataList", "BatchedQueries.GeometryList"};
int property_id(const std::string &name) {
  for (int i = 0; i < 14; ++i)
    if (name == properties[i])
      return i;
  return -1;
}
struct Problem {
  SceneQueryCode code;
  std::string text;
  std::size_t offset = 0;
};
void need(bool ok, SceneQueryCode code, const char *text,
          std::size_t offset = 0) {
  if (!ok)
    throw Problem{code, text, offset};
}
bool finite(Vec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
void direction(Vec3 p) {
  const double n = double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z;
  need(finite(p) && std::abs(n - 1) < 2e-4, SceneQueryCode::malformed,
       "query direction must be finite and unit length");
}
void transform(const Transform &t) {
  const auto &q = t.rotation;
  const double n = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z +
                   double(q.w) * q.w;
  need(finite(t.position) && std::isfinite(n) &&
           std::abs(std::sqrt(n) - 1) < 1e-4,
       SceneQueryCode::malformed,
       "query pose must be finite with a unit quaternion");
}
class Reader {
  const Bytes &bytes_;
  ByteOrder order_;
  std::size_t cursor_ = 0;

public:
  Reader(const PropertyValue &p)
      : bytes_(p.raw_bytes()), order_(p.byte_order) {}
  std::size_t position() const { return cursor_; }
  std::size_t remaining() const { return bytes_.size() - cursor_; }
  void seek(std::size_t p) {
    need(p <= bytes_.size(), SceneQueryCode::malformed,
         "truncated native structure", cursor_);
    cursor_ = p;
  }
  std::uint64_t integer(std::size_t n) {
    need(n <= remaining(), SceneQueryCode::malformed, "truncated native field",
         cursor_);
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const auto shift = order_ == ByteOrder::little ? i : n - i - 1;
      v |= std::uint64_t(bytes_[cursor_++]) << (8 * shift);
    }
    return v;
  }
  std::uint32_t u32() { return static_cast<std::uint32_t>(integer(4)); }
  float scalar() {
    const auto bits = u32();
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
  }
  Vec3 vec() { return {scalar(), scalar(), scalar()}; }
  Transform pose() {
    const Quat q{scalar(), scalar(), scalar(), scalar()};
    return {q, vec()};
  }
  SceneQueryFilterData filter() { return {u32(), u32(), u32(), u32()}; }
  SceneQueryArrayRange reference(std::size_t width) {
    integer(width);
    return {{}, u32(), u32()};
  }
  std::string text(std::size_t limit) {
    const auto begin = cursor_;
    while (remaining()) {
      const auto c = bytes_[cursor_++];
      if (!c)
        return std::string(
            reinterpret_cast<const char *>(bytes_.data() + begin),
            cursor_ - begin - 1);
      need(cursor_ - begin <= limit, SceneQueryCode::limit_exceeded,
           "query reference string exceeds limit", begin);
    }
    throw Problem{SceneQueryCode::malformed,
                  "unterminated query reference string", begin};
  }
};
void array(const PropertyValue &p, const char *type, const SceneQueryOptions &o,
           std::size_t max) {
  need(p.incoming_type == QualifiedName{"physx3", type},
       SceneQueryCode::invalid_type, "unexpected native query array datatype");
  need(p.pointer_width == 4 || p.pointer_width == 8,
       SceneQueryCode::invalid_type, "source pointer width must be 4 or 8");
  need(p.raw_bytes().size() <= o.max_array_bytes && p.item_count <= max,
       SceneQueryCode::limit_exceeded, "query array exceeds byte/item budget");
}
const PropertyValue &required(const InstanceState &scene, const char *name) {
  const auto *p = scene.property(name);
  need(p, SceneQueryCode::missing_array,
       "referenced query array was not recorded");
  return *p;
}
void bounds(const SceneQueryArrayRange &ref, const char *expected,
            std::uint64_t count) {
  need(ref.array_name == expected, SceneQueryCode::malformed,
       "query references an unexpected array name");
  need(ref.base_index <= count && ref.count <= count - ref.base_index,
       SceneQueryCode::out_of_range,
       "query array range exceeds recorded item count");
}
std::optional<ObjectKey> resolve(const Snapshot &snapshot, std::uint64_t id,
                                 const Metadata &meta, const char *type) {
  if (!id)
    return {};
  const auto *obj = snapshot.instance(id);
  need(obj && meta.is_a(obj->class_name, {"physx3", type}),
       SceneQueryCode::dangling_reference,
       "query object reference is missing or has the wrong class");
  return ObjectKey{obj->id, obj->generation};
}
float number(const InstanceState &object, const char *name) {
  const auto &p = required(object, name);
  const auto v = p.as_float();
  need(v && std::isfinite(*v), SceneQueryCode::malformed,
       "geometry scalar missing, wrongly typed or nonfinite");
  return *v;
}
Vec3 vector(const InstanceState &object, const char *name) {
  const auto &p = required(object, name);
  const auto v = p.as_vec3();
  need(v && finite(*v), SceneQueryCode::malformed,
       "geometry vector missing, wrongly typed or nonfinite");
  return *v;
}
void ensure_object(Snapshot &snapshot, const Session &session, std::uint64_t id) {
  if (!id || snapshot.instance(id)) return;
  auto object = session.instance_at_event(snapshot.event_count, id);
  if (object) snapshot.instances.emplace(id, std::move(*object));
}
SceneQueryGeometry geometry(Snapshot &snapshot, const Session &session,
                            const Metadata &meta, std::uint64_t id) {
  ensure_object(snapshot, session, id);
  const auto *p = snapshot.instance(id);
  need(p, SceneQueryCode::dangling_reference,
       "query geometry instance is missing at its publication");
  SceneQueryGeometry g;
  g.key = {p->id, p->generation};
  g.class_name = p->class_name;
  auto is = [&](const char *name) {
    return meta.is_a(p->class_name, {"physx3", name});
  };
  if (is("PxBoxGeometry")) {
    const auto v = vector(*p, "HalfExtents");
    need(v.x > 0 && v.y > 0 && v.z > 0, SceneQueryCode::malformed,
         "query box extents must be positive");
    g.value = Box{v};
  } else if (is("PxSphereGeometry")) {
    const auto r = number(*p, "Radius");
    need(r > 0, SceneQueryCode::malformed,
         "query sphere radius must be positive");
    g.value = Sphere{r};
  } else if (is("PxCapsuleGeometry")) {
    const auto r = number(*p, "Radius"), h = number(*p, "HalfHeight");
    need(r > 0 && h > 0, SceneQueryCode::malformed,
         "query capsule radius/half height are invalid");
    g.value = Capsule{r, h};
  } else if (is("PxConvexMeshGeometry")) {
    SceneQueryConvex c;
    const auto id_value = required(*p, "ConvexMesh").as_object_ref();
    need(bool(id_value), SceneQueryCode::invalid_type,
         "query convex mesh reference has the wrong type");
    c.resource_id = *id_value;
    ensure_object(snapshot, session, *id_value);
    c.resource = resolve(snapshot, *id_value, meta, "PxConvexMesh");
    need(c.resource.has_value(), SceneQueryCode::dangling_reference,
         "query convex mesh resource is null");
    c.scale.scale = vector(*p, "Scale.Scale");
    const auto q = required(*p, "Scale.Rotation").as_quat();
    need(q.has_value(), SceneQueryCode::invalid_type,
         "query convex scale rotation has the wrong type");
    c.scale.rotation = *q;
    transform({*q, {}});
    for (float v : {c.scale.scale.x, c.scale.scale.y, c.scale.scale.z})
      need(v >= 1e-6f && v <= 1e6f, SceneQueryCode::malformed,
           "query convex scales must be within the SDK range [1e-6,1e6]");
    const auto flags = required(*p, "MeshFlags").as_uint();
    need(flags && *flags <= UINT32_MAX, SceneQueryCode::invalid_type,
         "query convex flags have the wrong type");
    c.flags = static_cast<std::uint32_t>(*flags);
    g.value = c;
  } else
    throw Problem{SceneQueryCode::unsupported_geometry,
                  "unsupported scene-query geometry class", 0};
  return g;
}
std::size_t native_size(SceneQueryKind kind, std::size_t width) {
  if (kind == SceneQueryKind::raycast)
    return width == 8 ? 72 : 64;
  if (kind == SceneQueryKind::sweep)
    return width == 8 ? 88 : 72;
  return width == 8 ? 88 : 76;
}
SceneQuery read_query(Reader &r, const PropertyValue &value,
                      SceneQuerySource source,
                      const SceneQueryOptions &options) {
  SceneQuery q;
  q.source = source;
  const auto start = r.position();
  const auto width = value.pointer_width;
  const auto kind = source.kind;
  const auto size = native_size(kind, width);
  need(r.remaining() >= size, SceneQueryCode::malformed,
       "truncated query native structure", start);
  q.type = r.u32();
  if (kind == SceneQueryKind::sweep) {
    q.filter_flags = r.u32();
    q.unit_direction = r.vec();
    q.max_distance = r.scalar();
    q.geometry_range = r.reference(width);
    q.pose_range = r.reference(width);
    q.filter_data_range = r.reference(width);
    q.hits_range = r.reference(width);
  } else {
    q.filter_data.push_back(r.filter());
    q.filter_flags = r.u32();
    if (kind == SceneQueryKind::raycast) {
      q.origin = r.vec();
      q.unit_direction = r.vec();
      q.max_distance = r.scalar();
    } else
      q.overlap_pose = r.pose();
    r.seek(start + (width == 8 ? 56 : 52));
    if (kind == SceneQueryKind::overlap)
      q.geometry_range = r.reference(width);
    q.hits_range = r.reference(width);
  }
  r.seek(start + size);
  if (kind != SceneQueryKind::raycast) {
    q.geometry_range.array_name = r.text(options.max_reference_string_bytes);
    if (kind == SceneQueryKind::sweep) {
      q.pose_range.array_name = r.text(options.max_reference_string_bytes);
      q.filter_data_range.array_name =
          r.text(options.max_reference_string_bytes);
    }
  }
  q.hits_range.array_name = r.text(options.max_reference_string_bytes);
  return q;
}
} // namespace
const char *scene_query_array_name(SceneQueryKind kind, bool batched) noexcept {
  return properties[(batched ? 7 : 0) + static_cast<unsigned>(kind)];
}
struct SceneQueryIndex::Impl {
  struct Update {
    ObjectKey scene;
    int property;
    std::size_t prefix;
    std::uint64_t stream, timestamp, offset;
    bool coordinates_ambiguous = false;
    std::uint64_t origin_epoch = 0;
  };
  struct Pending {
    ObjectKey scene;
    int property;
  };
  SceneQueryOptions options;
  const Session *bound_session = nullptr;
  std::size_t indexed = 0;
  bool limited = false;
  std::map<std::pair<std::uint64_t, std::uint32_t>, int> names;
  std::map<std::uint64_t, std::uint64_t> generations, alive;
  std::map<std::uint64_t, Pending> pending;
  std::vector<Update> updates;
  struct Epochs {
    std::uint64_t current = 0;
    std::array<std::uint64_t, 6> published{};
  };
  std::map<std::uint64_t, Epochs> origin_epochs;
  explicit Impl(SceneQueryOptions o) : options(o) {
    if (!std::isfinite(o.normal_length) || o.normal_length < 0 ||
        !o.max_diagnostics || !o.max_reference_string_bytes)
      throw std::invalid_argument(
          "query normal length must be finite and nonnegative; "
          "string/diagnostic limits must be nonzero");
  }
  std::size_t entries() const {
    return names.size() + generations.size() + alive.size() + pending.size() +
           updates.size() + origin_epochs.size();
  }
  int name(std::uint64_t stream, std::uint32_t handle) const {
    const auto it = names.find({stream, handle});
    return it == names.end() ? -1 : it->second;
  }
  void record(std::uint64_t id, int property, const Event &e,
              std::size_t prefix) {
    if (property < 0)
      return;
    const auto it = alive.find(id);
    if (it != alive.end()) {
      bool ambiguous = false;
      auto &epochs = origin_epochs[id];
      if (property % 7 < 3) {
        const auto slot = (property / 7) * 3 + property % 7;
        ambiguous = epochs.published[slot] != epochs.current;
        epochs.published[slot] = epochs.current;
      }
      updates.push_back({{id, it->second},
                         property,
                         prefix,
                         e.stream_id,
                         e.timestamp,
                         e.byte_offset, ambiguous, epochs.current});
    }
  }
  void update(const Session &s, std::size_t end) {
    if (!bound_session)
      bound_session = &s;
    else if (bound_session != &s)
      throw std::invalid_argument(
          "reset SceneQueryIndex before binding a new Session");
    if (end > s.event_count())
      throw std::out_of_range("query index prefix exceeds Session");
    if (limited)
      return;
    while (indexed < end) {
      if (entries() + 3 > options.max_index_entries) {
        limited = true;
        break;
      }
      const auto &e = s.events()[indexed];
      const auto prefix = indexed + 1;
      std::visit(
          [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, StringHandleEvent>) {
              const auto property = property_id(v.string);
              if (property >= 0)
                names[{e.stream_id, v.handle}] = property;
            } else if constexpr (std::is_same_v<T, CreateInstance>) {
              alive[v.instance_id] = generations[v.instance_id]++;
              origin_epochs.erase(v.instance_id);
            } else if constexpr (std::is_same_v<T, DestroyInstance>) {
              alive.erase(v.instance_id);
              origin_epochs.erase(v.instance_id);
            } else if constexpr (std::is_same_v<T, OriginShift>)
              ++origin_epochs[v.instance_id].current;
            else if constexpr (std::is_same_v<T, SetPropertyValue>)
              record(v.instance_id, name(e.stream_id, v.property_name), e,
                     prefix);
            else if constexpr (std::is_same_v<T, BeginSetPropertyValue>) {
              const auto property = name(e.stream_id, v.property_name);
              if (property >= 0) {
                const auto it = alive.find(v.instance_id);
                if (it != alive.end())
                  pending[e.stream_id] = {{v.instance_id, it->second},
                                          property};
              }
            } else if constexpr (std::is_same_v<T, EndSetPropertyValue>) {
              auto p = pending.find(e.stream_id);
              if (p != pending.end()) {
                // Core assigns a completed transaction to the instance alive
                // at End, even if another stream recreated this raw ID after
                // Begin. Match that publication generation.
                record(p->second.scene.id, p->second.property, e, prefix);
                pending.erase(p);
              }
            } else if constexpr (std::is_same_v<T, PushBackObjectRef> ||
                                 std::is_same_v<T, RemoveObjectRef>)
              record(v.instance_id, name(e.stream_id, v.property), e, prefix);
          },
          e.payload);
      indexed = prefix;
    }
  }
  SceneQuerySource provenance(const Update &update, SceneQueryKind kind,
                              bool batched, std::size_t sequence = 0) const {
    return {update.scene,  kind,          batched,       sequence,
            update.prefix, update.prefix, update.stream, update.timestamp,
            update.offset};
  }
  void diagnostic(SceneQueries &out, SceneQueryCode code,
                  const SceneQuerySource &source, const std::string &property,
                  const std::string &text, std::size_t offset = 0) const {
    out.complete = false;
    if (out.diagnostics.size() < options.max_diagnostics)
      out.diagnostics.push_back({code, source, property, text, offset});
  }
  bool fresh(ObjectKey scene, int property, std::size_t begin,
             std::size_t end) const {
    auto first = std::upper_bound(
        updates.begin(), updates.end(), begin,
        [](std::size_t p, const Update &u) { return p < u.prefix; });
    for (auto it = first; it != updates.end() && it->prefix <= end; ++it)
      if (it->scene == scene && it->property == property)
        return true;
    return false;
  }
  void referenced(const SceneQueryArrayRange &range, int property,
                  const InstanceState &scene, const SceneQuerySource &source,
                  std::size_t begin) const {
    need(range.array_name == properties[property], SceneQueryCode::malformed,
         "query references an unexpected array family");
    if (range.count)
      need(fresh(source.scene, property, begin, source.publication_event_count),
           SceneQueryCode::stale_array,
           "referenced array has no fresh publication in this window");
    const auto &p = required(scene, properties[property]);
    bounds(range, properties[property], p.item_count);
  }
  void attach(SceneQuery &q, Snapshot &snapshot, const Session &session,
              const Metadata &meta,
              const InstanceState &scene, std::size_t begin,
              std::size_t &total_hits, std::size_t &total_aux,
              SceneQueries &output) const {
    const auto kind = q.source.kind;
    const int base = q.source.batched ? 7 : 0;
    const std::uint64_t aux_required =
        kind == SceneQueryKind::raycast ? q.filter_data.size()
        : kind == SceneQueryKind::overlap
            ? std::uint64_t(q.geometry_range.count) + 1 + q.filter_data.size()
            : std::uint64_t(q.geometry_range.count) + q.pose_range.count +
                  q.filter_data_range.count;
    need(aux_required <= options.max_aux_items - total_aux,
         SceneQueryCode::limit_exceeded,
         "total query geometry/pose/filter output exceeds auxiliary item "
         "budget");
    auto commit_counts = [&] {
      total_hits += q.hits.size();
      total_aux += static_cast<std::size_t>(aux_required);
    };
    need((kind == SceneQueryKind::raycast && q.type <= 2) ||
             (kind == SceneQueryKind::overlap && q.type >= 3 && q.type <= 7) ||
             (kind == SceneQueryKind::sweep && q.type >= 8 && q.type <= 10),
         SceneQueryCode::unsupported_query,
         "raw query type does not match the recorded query family");
    if (q.origin)
      need(finite(*q.origin), SceneQueryCode::malformed,
           "query origin is nonfinite");
    if (q.unit_direction)
      direction(*q.unit_direction);
    if (q.max_distance)
      need(std::isfinite(*q.max_distance) && *q.max_distance >= 0,
           SceneQueryCode::malformed,
           "query max distance must be finite and nonnegative");
    if (q.overlap_pose)
      transform(*q.overlap_pose);
    if (q.filter_flags & (1u << 5))
      q.has_block = false;
    referenced(q.hits_range, base + 3, scene, q.source, begin);
    const auto &hp = required(scene, properties[base + 3]);
    array(hp, "PvdSqHit", options, options.max_hits);
    const auto stride = hp.pointer_width == 8 ? 64u : 52u;
    need(hp.item_count <= hp.raw_bytes().size() / stride &&
             hp.raw_bytes().size() == hp.item_count * stride,
         SceneQueryCode::malformed,
         "PvdSqHit array has wrong source-ABI stride");
    need(q.hits_range.count <= options.max_hits - total_hits,
         SceneQueryCode::limit_exceeded, "output query hits exceed budget");
    Reader hits(hp);
    hits.seek(std::size_t(q.hits_range.base_index) * stride);
    for (std::uint32_t i = 0; i < q.hits_range.count; ++i) {
      const auto start = hits.position();
      SceneQueryHit h;
      h.shape_id = hits.integer(hp.pointer_width);
      h.actor_id = hits.integer(hp.pointer_width);
      h.face_index = hits.u32();
      h.flags = hits.u32();
      h.position = hits.vec();
      h.normal = hits.vec();
      h.distance = hits.scalar();
      h.u = hits.scalar();
      h.v = hits.scalar();
      hits.seek(start + stride);
      h.position_available = kind != SceneQueryKind::overlap && (h.flags & 1);
      h.normal_available = kind != SceneQueryKind::overlap && (h.flags & 2);
      h.distance_available = kind != SceneQueryKind::overlap;
      h.uv_available = kind == SceneQueryKind::raycast && (h.flags & 8);
      h.face_index_available =
          kind != SceneQueryKind::overlap && (h.flags & (1u << 10));
      need(!h.position_available || finite(h.position),
           SceneQueryCode::malformed, "available hit position is nonfinite",
           start);
      need(!h.normal_available || finite(h.normal), SceneQueryCode::malformed,
           "available hit normal is nonfinite", start);
      need(!h.distance_available || std::isfinite(h.distance),
           SceneQueryCode::malformed, "available hit distance is nonfinite",
           start);
      need(!h.uv_available || (std::isfinite(h.u) && std::isfinite(h.v)),
           SceneQueryCode::malformed, "available hit UV is nonfinite", start);
      auto object = [&](std::uint64_t id, const char *type) {
        try {
          ensure_object(snapshot, session, id);
          return resolve(snapshot, id, meta, type);
        } catch (const Problem &problem) {
          q.complete = false;
          diagnostic(output, problem.code, q.source, properties[base + 3],
                     problem.text, start);
          return std::optional<ObjectKey>{};
        }
      };
      h.shape = object(h.shape_id, "PxShape");
      h.actor = object(h.actor_id, "PxActor");
      if (h.actor) {
        const auto *actor = snapshot.instance(h.actor->id);
        const auto *owner = actor->property("Scene");
        const auto owner_id =
            owner ? owner->as_object_ref() : std::optional<std::uint64_t>{};
        if (owner_id && *owner_id && *owner_id != q.source.scene.id) {
          q.complete = false;
          diagnostic(output, SceneQueryCode::malformed, q.source,
                     properties[base + 3],
                     "hit actor belongs to a different recorded scene", start);
        }
      }

      if (q.filter_flags & (1u << 5))
        h.role = SceneQueryHitRole::touching;
      q.hits.push_back(h);
    }
    if (kind == SceneQueryKind::raycast) {
      commit_counts();
      return;
    }
    referenced(q.geometry_range, base + 6, scene, q.source, begin);
    const auto &gp = required(scene, properties[base + 6]);
    need(gp.reference_ids && gp.reference_ids->size() == gp.item_count,
         SceneQueryCode::invalid_type,
         "geometry list must be an ObjectRef array");
    need(gp.item_count <= options.max_aux_items &&
             gp.raw_bytes().size() <= options.max_array_bytes,
         SceneQueryCode::limit_exceeded,
         "query geometry list exceeds byte/item budget");
    need(q.geometry_range.count != 0, SceneQueryCode::malformed,
         "query geometry range is empty");
    for (std::uint32_t i = 0; i < q.geometry_range.count; ++i) {
      auto g = geometry(
          snapshot, session, meta,
          gp.reference_ids->at(std::size_t(q.geometry_range.base_index) + i));
      const auto variant = g.value.index();
      bool matches = true;
      if (kind == SceneQueryKind::overlap)
        matches = (q.type == 3 && variant == 1) ||
                  ((q.type == 4 || q.type == 5) && variant == 0) ||
                  (q.type == 6 && variant == 2) ||
                  (q.type == 7 && variant == 3);
      else
        matches = (q.type == 8 && variant == 0) ||
                  (q.type == 9 && (variant == 1 || variant == 2)) ||
                  (q.type == 10 && variant == 3);
      need(matches, SceneQueryCode::malformed,
           "query type and recorded geometry disagree");
      q.geometries.push_back(std::move(g));
    }
    if (kind == SceneQueryKind::overlap) {
      q.poses.push_back(*q.overlap_pose);
      commit_counts();
      return;
    }
    referenced(q.pose_range, base + 4, scene, q.source, begin);
    referenced(q.filter_data_range, base + 5, scene, q.source, begin);
    need(q.geometry_range.count == q.pose_range.count &&
             q.pose_range.count == q.filter_data_range.count,
         SceneQueryCode::malformed,
         "sweep geometry/pose/filter array counts disagree");
    const auto &pp = required(scene, properties[base + 4]);
    array(pp, "PxTransform", options, options.max_aux_items);
    need(pp.item_count <= pp.raw_bytes().size() / 28 &&
             pp.raw_bytes().size() == pp.item_count * 28,
         SceneQueryCode::malformed, "pose list has wrong stride");
    Reader poses(pp);
    poses.seek(std::size_t(q.pose_range.base_index) * 28);
    for (std::uint32_t i = 0; i < q.pose_range.count; ++i) {
      auto p = poses.pose();
      transform(p);
      q.poses.push_back(p);
    }
    const auto &fp = required(scene, properties[base + 5]);
    array(fp, "U32Array4", options, options.max_aux_items);
    need(fp.item_count <= fp.raw_bytes().size() / 16 &&
             fp.raw_bytes().size() == fp.item_count * 16,
         SceneQueryCode::malformed, "filter list has wrong stride");
    Reader filters(fp);
    filters.seek(std::size_t(q.filter_data_range.base_index) * 16);
    for (std::uint32_t i = 0; i < q.filter_data_range.count; ++i)
      q.filter_data.push_back(filters.filter());
    commit_counts();
  }
  void lines(SceneQueries &out, const SceneQuery &q, bool &line_limited) const {
    auto add = [&](Vec3 a, Vec3 b, SceneQueryLineKind kind,
                   std::optional<std::size_t> hit, std::uint32_t color) {
      if (!finite(a) || !finite(b)) {
        diagnostic(out, SceneQueryCode::malformed, q.source,
                   scene_query_array_name(q.source.kind, q.source.batched),
                   "derived query display line overflowed");
        return;
      }
      if (out.lines.size() >= options.max_lines) {
        if (!line_limited) {
          line_limited = true;
          diagnostic(out, SceneQueryCode::limit_exceeded, q.source, "display",
                     "query line budget exceeded");
        }
        return;
      }
      out.lines.push_back({q.source, kind, hit, a, b, color, true});
    };
    if (options.emit_paths && q.unit_direction && q.max_distance) {
      const auto d = *q.unit_direction;
      const auto distance = *q.max_distance;
      auto path = [&](Vec3 p) {
        add(p,
            {p.x + d.x * distance, p.y + d.y * distance, p.z + d.z * distance},
            SceneQueryLineKind::requested_path, {}, 0xff34c9c0u);
      };
      if (q.origin)
        path(*q.origin);
      else
        for (const auto &p : q.poses)
          path(p.position);
    }
    if (options.emit_normals && options.normal_length > 0)
      for (std::size_t i = 0; i < q.hits.size(); ++i) {
        const auto &h = q.hits[i];
        if (h.position_available && h.normal_available)
          add(h.position,
              {h.position.x + h.normal.x * options.normal_length,
               h.position.y + h.normal.y * options.normal_length,
               h.position.z + h.normal.z * options.normal_length},
              SceneQueryLineKind::impact_normal, i, 0xfff2c94cu);
      }
  }
  SceneQueries extract(const Session &session, SceneQueryWindow window,
                       std::optional<std::uint64_t> frame_stream = {}) {
    SceneQueries out;
    out.scene = {window.scene_id, 0};
    out.begin_event_count = window.begin_event_count;
    out.event_count = window.end_event_count;
    for (int b = 0; b < 2; ++b)
      for (int k = 0; k < 3; ++k) {
        auto &c = out.collections[b * 3 + k];
        c.kind = static_cast<SceneQueryKind>(k);
        c.batched = b != 0;
      }
    SceneQuerySource fallback;
    fallback.scene = out.scene;
    fallback.snapshot_event_count = window.end_event_count;
    if (window.begin_event_count > window.end_event_count ||
        window.end_event_count > session.event_count()) {
      diagnostic(out, SceneQueryCode::invalid_range, fallback, "",
                 "invalid scene-query event window");
      return out;
    }
    update(session, window.end_event_count);
    if (indexed < window.end_event_count) {
      diagnostic(out, SceneQueryCode::limit_exceeded, fallback, "index",
                 "query publication index exceeds entry budget");
      return out;
    }
    const auto ending_scene =
        session.instance_at_event(window.end_event_count, window.scene_id);
    const auto *scene = ending_scene ? &*ending_scene : nullptr;
    if (!scene ||
        !session.metadata().is_a(scene->class_name, {"physx3", "PxScene"})) {
      diagnostic(out, SceneQueryCode::missing_scene, fallback, "",
                 "requested PxScene is not alive at the window end");
      return out;
    }
    out.scene = {scene->id, scene->generation};
    std::array<const Update *, 6> selected{};
    std::array<std::size_t, 6> publications{};
    auto first = std::upper_bound(
        updates.begin(), updates.end(), window.begin_event_count,
        [](std::size_t p, const Update &u) { return p < u.prefix; });
    for (auto it = first;
         it != updates.end() && it->prefix <= window.end_event_count; ++it) {
      if (!(it->scene == out.scene) || it->property % 7 >= 3 ||
          (frame_stream && it->stream != *frame_stream))
        continue;
      const auto slot = (it->property / 7) * 3 + it->property % 7;
      selected[slot] = &*it;
      ++publications[slot];
    }
    std::size_t total_hits = 0, total_aux = 0;
    bool line_limited = false;
    for (std::size_t slot = 0; slot < 6; ++slot) {
      if (!selected[slot])
        continue;
      const auto &u = *selected[slot];
      auto &collection = out.collections[slot];
      collection.state = SceneQueryCollectionState::recorded;
      collection.publication_event_count = u.prefix;
      auto source_value = provenance(u, collection.kind, collection.batched);
      if (publications[slot] > 1) {
        collection.state = SceneQueryCollectionState::malformed;
        diagnostic(out, SceneQueryCode::multiple_publications, source_value,
                   properties[u.property],
                   "window contains multiple publications of this family; only "
                   "the latest is returned");
      }
      auto snapshot = session.snapshot_selected_at_event(u.prefix, {window.scene_id});
      const auto *owner = snapshot.instance(window.scene_id);
      const auto hits_before = total_hits, aux_before = total_aux;
      try {
        need(owner && owner->generation == out.scene.generation,
             SceneQueryCode::missing_scene,
             "query publication scene generation is unavailable");
        const auto &p = required(*owner, properties[u.property]);
        const char *types[] = {"PvdRaycast", "PvdSweep", "PvdOverlap"};
        array(p, types[static_cast<unsigned>(collection.kind)], options,
              options.max_queries - out.queries.size());
        Reader reader(p);
        std::vector<SceneQuery> staged;
        staged.reserve(static_cast<std::size_t>(p.item_count));
        for (std::size_t i = 0; i < p.item_count; ++i) {
          auto item_source = source_value;
          item_source.sequence = i;
          const auto start = reader.position();
          auto q = read_query(reader, p, item_source, options);
          q.coordinates_ambiguous = u.coordinates_ambiguous ||
                                     u.origin_epoch != scene->coordinate_epoch;
          try {
            attach(q, snapshot, session, session.metadata(), *owner,
                   window.begin_event_count, total_hits, total_aux, out);
          } catch (const Problem &problem) {
            q.complete = false;
            q.hits.clear();
            q.geometries.clear();
            q.poses.clear();
            q.filter_data.clear();
            collection.state = SceneQueryCollectionState::malformed;
            diagnostic(out, problem.code, q.source, properties[u.property],
                       problem.text, problem.offset ? problem.offset : start);
          }
          if (!q.complete)
            collection.state = SceneQueryCollectionState::malformed;
          staged.push_back(std::move(q));
        }
        need(reader.remaining() == 0, SceneQueryCode::malformed,
             "query native array contains trailing bytes", reader.position());
        collection.query_count = staged.size();
        if (!staged.empty() && staged.front().coordinates_ambiguous)
          diagnostic(out, SceneQueryCode::ambiguous_coordinates, source_value,
                     properties[u.property],
                     "OriginShift overlaps the query collection/publication "
                     "interval. PhysX 3.4 sends no per-query origin epoch; raw "
                     "coordinates are retained and spatial previews suppressed.");
        for (auto &q : staged) {
          if (q.complete && !q.coordinates_ambiguous)
            lines(out, q, line_limited);
          out.queries.push_back(std::move(q));
        }
      } catch (const Problem &problem) {
        total_hits = hits_before;
        total_aux = aux_before;
        collection.state = SceneQueryCollectionState::malformed;
        diagnostic(out, problem.code, source_value, properties[u.property],
                   problem.text, problem.offset);
      }
    }
    return out;
  }
};
SceneQueryIndex::SceneQueryIndex(SceneQueryOptions o)
    : impl_(std::make_unique<Impl>(o)) {}
SceneQueryIndex::~SceneQueryIndex() = default;
SceneQueryIndex::SceneQueryIndex(SceneQueryIndex &&) noexcept = default;
SceneQueryIndex &
SceneQueryIndex::operator=(SceneQueryIndex &&) noexcept = default;
void SceneQueryIndex::reset() {
  impl_ = std::make_unique<Impl>(impl_->options);
}
void SceneQueryIndex::update(const Session &s, std::size_t prefix) {
  impl_->update(s, prefix == std::numeric_limits<std::size_t>::max()
                       ? s.event_count()
                       : prefix);
}
std::size_t SceneQueryIndex::indexed_event_count() const noexcept {
  return impl_->indexed;
}
SceneQueries SceneQueryIndex::extract(const Session &s, SceneQueryWindow w) {
  return impl_->extract(s, w);
}
SceneQueries SceneQueryIndex::extract(const Session &s, SceneQueryWindow w,
                                      std::uint64_t stream_id) {
  return impl_->extract(s, w, stream_id);
}
SceneQueries SceneQueryIndex::extract(const Session &s, const FrameMarker &f) {
  return extract(
      s, SceneQueryWindow{f.scene_id, f.begin_event_count, f.event_count});
}
SceneQueries extract_scene_queries(const Session &s, SceneQueryWindow w,
                                   SceneQueryOptions o) {
  SceneQueryIndex index(o);
  return index.extract(s, w);
}
SceneQueries extract_scene_queries(const Session &s, const FrameMarker &f,
                                   SceneQueryOptions o) {
  SceneQueryIndex index(o);
  return index.extract(s, f);
}
} // namespace pvd::render
