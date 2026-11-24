#include "pvd/render.hpp"
#include "pvd/coordinates.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace pvd::render {
namespace {
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(Vec3 a, float f) { return {a.x * f, a.y * f, a.z * f}; }
Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
bool finite(Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool unit(Quat q) {
  double n = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z +
             double(q.w) * q.w;
  return std::isfinite(n) && std::abs(n - 1.0) <= 1e-3;
}
struct Failure {
  DiagnosticCode code;
  std::uint64_t id;
  std::string property, message;
};
[[noreturn]] void fail(DiagnosticCode c, const InstanceState &o,
                       const std::string &p, const std::string &m) {
  throw Failure{c, o.id, p, m};
}
const PropertyValue &need(const InstanceState &o, const std::string &name) {
  auto p = o.property(name);
  if (!p)
    fail(DiagnosticCode::missing_property, o, name,
         "required property is absent");
  return *p;
}
bool type(const PropertyValue &v, const char *n) {
  return v.incoming_type == QualifiedName{"physx3", n};
}
void size_is(const InstanceState &o, const std::string &name,
             const PropertyValue &p, std::uint64_t width) {
  if (p.item_count > std::numeric_limits<std::size_t>::max() / width ||
      p.raw_bytes().size() != p.item_count * width)
    fail(DiagnosticCode::malformed_payload, o, name,
         "byte length differs from item count and source type");
}
void scalar(const InstanceState &o, const std::string &name,
            const PropertyValue &v) {
  if (v.item_count != 1)
    fail(DiagnosticCode::count_mismatch, o, name, "expected one scalar item");
}
std::uint64_t uint_at(const InstanceState &o, const std::string &name,
                      const PropertyValue &v, std::size_t offset,
                      std::size_t width) {
  const auto &b = v.raw_bytes();
  if (offset > b.size() || width > b.size() - offset)
    fail(DiagnosticCode::malformed_payload, o, name,
         "raw field exceeds payload");
  std::uint64_t n = 0;
  for (std::size_t i = 0; i < width; ++i) {
    auto j = v.byte_order == ByteOrder::little ? i : width - 1 - i;
    n |= std::uint64_t(b[offset + i]) << (j * 8);
  }
  return n;
}
std::uint64_t integer(const InstanceState &o, const std::string &name) {
  const auto &p = need(o, name);
  scalar(o, name, p);
  std::size_t width = 0;
  if (type(p, "PvdU8") || type(p, "PvdBool"))
    width = 1;
  else if (type(p, "PvdU16"))
    width = 2;
  else if (type(p, "PvdU32"))
    width = 4;
  else if (type(p, "PvdU64"))
    width = 8;
  if (!width)
    fail(DiagnosticCode::invalid_type, o, name,
         "expected unsigned integer or boolean");
  size_is(o, name, p, width);
  return uint_at(o, name, p, 0, width);
}
float number(const InstanceState &o, const std::string &name) {
  const auto &p = need(o, name);
  scalar(o, name, p);
  if (!type(p, "PvdF32"))
    fail(DiagnosticCode::invalid_type, o, name, "expected PvdF32");
  size_is(o, name, p, 4);
  auto n = *p.as_float();
  if (!std::isfinite(n))
    fail(DiagnosticCode::invalid_value, o, name, "non-finite scalar");
  return n;
}
Vec3 vector(const InstanceState &o, const std::string &name) {
  const auto &p = need(o, name);
  scalar(o, name, p);
  if (!type(p, "PxVec3"))
    fail(DiagnosticCode::invalid_type, o, name, "expected PxVec3");
  size_is(o, name, p, 12);
  auto v = *p.as_vec3();
  if (!finite(v))
    fail(DiagnosticCode::invalid_value, o, name, "non-finite vector");
  return v;
}
Quat quaternion(const InstanceState &o, const std::string &name) {
  const auto &p = need(o, name);
  scalar(o, name, p);
  if (!type(p, "PxQuat"))
    fail(DiagnosticCode::invalid_type, o, name, "expected PxQuat");
  size_is(o, name, p, 16);
  auto q = *p.as_quat();
  if (!unit(q))
    fail(DiagnosticCode::invalid_value, o, name,
         "quaternion is non-finite or not unit length");
  return q;
}
Transform pose(const InstanceState &o, const std::string &name) {
  const auto &p = need(o, name);
  scalar(o, name, p);
  if (!type(p, "PxTransform"))
    fail(DiagnosticCode::invalid_type, o, name, "expected PxTransform");
  size_is(o, name, p, 28);
  auto t = *coordinates::native_transform(p);
  if (!finite(t.position) || !unit(t.rotation))
    fail(DiagnosticCode::invalid_value, o, name,
         "transform is non-finite or quaternion is not unit length");
  return t;
}
std::vector<std::uint64_t> references(const InstanceState &o,
                                      const std::string &name) {
  const auto &p = need(o, name);
  if (p.reference_ids) {
    if (p.reference_ids->size() != p.item_count)
      fail(DiagnosticCode::count_mismatch, o, name,
           "reference count differs from item count");
    return *p.reference_ids;
  }
  if (!type(p, "VoidPtr") || (p.pointer_width != 4 && p.pointer_width != 8))
    fail(DiagnosticCode::invalid_type, o, name,
         "reference is not a decoded source pointer; internal ObjectRef IDs "
         "are unsupported");
  size_is(o, name, p, p.pointer_width);
  std::vector<std::uint64_t> ids;
  ids.reserve(static_cast<std::size_t>(p.item_count));
  for (std::size_t i = 0; i < p.item_count; ++i)
    ids.push_back(uint_at(o, name, p, i * p.pointer_width, p.pointer_width));
  return ids;
}
std::uint64_t reference(const InstanceState &o, const std::string &name) {
  auto ids = references(o, name);
  if (ids.size() != 1)
    fail(DiagnosticCode::count_mismatch, o, name,
         "expected one object reference");
  return ids[0];
}
template <class SnapshotLike>
const InstanceState &resolve(const SnapshotLike &s, const InstanceState &o,
                             const std::string &p, std::uint64_t id) {
  auto ptr = s.instance(id);
  if (!ptr)
    fail(DiagnosticCode::dangling_reference, o, p,
         "referenced instance is absent: " + std::to_string(id));
  return *ptr;
}
MeshScale scale(const InstanceState &o, bool convex) {
  MeshScale s{vector(o, "Scale.Scale"), quaternion(o, "Scale.Rotation")};
  for (auto value : {s.scale.x, s.scale.y, s.scale.z})
    if ((convex && value <= 0) || std::abs(value) < 1e-6f ||
        std::abs(value) > 1e6f)
      fail(DiagnosticCode::invalid_value, o, "Scale.Scale",
           "mesh scale is outside the SDK range");
  return s;
}
void positive(float n, const InstanceState &o, const std::string &p,
              bool zero = false) {
  if (zero ? n < 0 : n <= 0)
    fail(DiagnosticCode::invalid_value, o, p,
         "geometry dimension must be positive");
}
std::optional<Bounds> bounds(const Geometry &g, const Transform &world) {
  if (std::holds_alternative<Plane>(g))
    return {};
  Bounds b;
  bool initialized = false;
  auto include = [&](Vec3 p) {
    p = transform_point(world, p);
    if (!finite(p))
      throw std::overflow_error("non-finite transformed geometry bounds");
    if (!initialized) {
      b.minimum = b.maximum = p;
      initialized = true;
    } else {
      b.minimum = {std::min(b.minimum.x, p.x), std::min(b.minimum.y, p.y),
                   std::min(b.minimum.z, p.z)};
      b.maximum = {std::max(b.maximum.x, p.x), std::max(b.maximum.y, p.y),
                   std::max(b.maximum.z, p.z)};
    }
  };
  std::visit(
      [&](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Box>) {
          for (int x : {-1, 1})
            for (int y : {-1, 1})
              for (int z : {-1, 1})
                include({v.half_extents.x * x, v.half_extents.y * y,
                         v.half_extents.z * z});
        } else if constexpr (std::is_same_v<T, Sphere>) {
          auto c = world.position;
          b = {{c.x - v.radius, c.y - v.radius, c.z - v.radius},
               {c.x + v.radius, c.y + v.radius, c.z + v.radius}};
          initialized = true;
        } else if constexpr (std::is_same_v<T, Capsule>) {
          auto x = rotate(world.rotation, {v.half_height, 0, 0});
          Vec3 e{std::abs(x.x) + v.radius, std::abs(x.y) + v.radius,
                 std::abs(x.z) + v.radius};
          b = {add(world.position, mul(e, -1)), add(world.position, e)};
          initialized = true;
        } else if constexpr (std::is_same_v<T, ConvexMesh> ||
                             std::is_same_v<T, TriangleMesh>) {
          for (auto p : v.mesh->vertices)
            include(apply_mesh_scale(v.scale, p));
        } else if constexpr (std::is_same_v<T, HeightField>) {
          for (auto p : v.mesh->vertices)
            include({p.x * v.row_scale, p.y * v.height_scale,
                     p.z * v.column_scale});
        }
      },
      g);
  if (initialized && (!finite(b.minimum) || !finite(b.maximum)))
    throw std::overflow_error("non-finite geometry bounds");
  return initialized ? std::optional<Bounds>(b) : std::nullopt;
}
} // namespace
Vec3 rotate(const Quat &q, const Vec3 &p) {
  auto t = mul(cross({q.x, q.y, q.z}, p), 2);
  return add(p, add(mul(t, q.w), cross({q.x, q.y, q.z}, t)));
}
Vec3 transform_point(const Transform &t, const Vec3 &p) {
  return add(t.position, rotate(t.rotation, p));
}
Transform compose(const Transform &a, const Transform &b) {
  const auto &q = a.rotation;
  const auto &r = b.rotation;
  return {{q.w * r.x + q.x * r.w + q.y * r.z - q.z * r.y,
           q.w * r.y - q.x * r.z + q.y * r.w + q.z * r.x,
           q.w * r.z + q.x * r.y - q.y * r.x + q.z * r.w,
           q.w * r.w - q.x * r.x - q.y * r.y - q.z * r.z},
          transform_point(a, b.position)};
}
Vec3 apply_mesh_scale(const MeshScale &s, const Vec3 &p) {
  auto v = rotate(s.rotation, p);
  return rotate(conjugate(s.rotation),
                {v.x * s.scale.x, v.y * s.scale.y, v.z * s.scale.z});
}

struct Extractor::Impl {
  struct Signature {
    std::vector<std::pair<std::string, PropertyValue>> values;
    bool operator==(const Signature &o) const {
      if (values.size() != o.values.size())
        return false;
      for (std::size_t i = 0; i < values.size(); ++i) {
        const auto &a = values[i];
        const auto &b = o.values[i];
        if (a.first != b.first || a.second.bytes != b.second.bytes ||
            a.second.incoming_type != b.second.incoming_type ||
            a.second.item_count != b.second.item_count ||
            a.second.byte_order != b.second.byte_order ||
            a.second.pointer_width != b.second.pointer_width)
          return false;
      }
      return true;
    }
  };
  struct Entry {
    Signature signature;
    std::shared_ptr<const MeshData> mesh;
    std::uint64_t used = 0;
  };
  struct WeakEntry {
    struct Input {
      std::string property;
      std::weak_ptr<const Bytes> bytes;
      QualifiedName incoming_type;
      std::uint64_t item_count;
      ByteOrder byte_order;
      std::uint8_t pointer_width;
    };
    std::vector<Input> inputs;
    std::weak_ptr<const MeshData> mesh;
    bool matches(const Signature &signature) const {
      if (inputs.size() != signature.values.size()) return false;
      for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto &a = inputs[i];
        const auto &b = signature.values[i];
        if (a.property != b.first || a.bytes.lock() != b.second.bytes ||
            a.incoming_type != b.second.incoming_type || a.item_count != b.second.item_count ||
            a.byte_order != b.second.byte_order || a.pointer_width != b.second.pointer_width)
          return false;
      }
      return true;
    }
  };
  using Key = std::tuple<std::uint64_t, std::uint64_t, GeometryKind>;
  ExtractorOptions options;
  std::map<Key, Entry> cache;
  // Returned Frames already own these resources. Remember their signatures
  // without extending mesh lifetimes beyond the bounded strong LRU.
  std::map<Key, WeakEntry> live_meshes;
  struct BoundsEntry {
    std::weak_ptr<const MeshData> mesh;
    std::array<float, 14> transform{};
    std::optional<Bounds> value;
  };
  static constexpr unsigned bounds_cache_bits = 12;
  using BoundsCache = std::array<BoundsEntry, 1u << bounds_cache_bits>;
  std::unique_ptr<BoundsCache> bounds_cache;
  std::uint64_t clock = 0;
  std::size_t hits = 0, misses = 0;
  std::map<QualifiedName, std::map<std::string_view, bool>> class_checks;
  // This memo is cleared at every public extraction call: Metadata is a
  // caller-owned mutable value, so its address cannot be a version token.
  bool is_a(const Metadata &metadata, const InstanceState &object, const char *name) {
    if (object.class_name.namespace_name == "physx3" && object.class_name.name == name)
      return true;
    auto &checks = class_checks[object.class_name];
    const auto found = checks.find(name);
    if (found != checks.end()) return found->second;
    const auto result = metadata.is_a(object.class_name, {"physx3", name});
    checks.emplace(name, result);
    return result;
  }
  void prune_live_meshes() {
    for (auto it = live_meshes.begin(); it != live_meshes.end();) {
      if (it->second.mesh.expired())
        it = live_meshes.erase(it);
      else
        ++it;
    }
  }
  void retain(const Key &key, const Signature &signature,
              const std::shared_ptr<const MeshData> &data) {
    if (!options.max_cache_entries) return;
    WeakEntry weak;
    weak.mesh = data;
    weak.inputs.reserve(signature.values.size());
    for (const auto &value : signature.values)
      weak.inputs.push_back({value.first, value.second.bytes, value.second.incoming_type,
          value.second.item_count, value.second.byte_order, value.second.pointer_width});
    live_meshes[key] = std::move(weak);
    cache[key] = {signature, data, ++clock};
    while (cache.size() > options.max_cache_entries) {
      auto oldest = std::min_element(cache.begin(), cache.end(),
          [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
      cache.erase(oldest);
    }
  }
  explicit Impl(ExtractorOptions o) : options(o) {
    if (!o.max_vertices || !o.max_triangles || !o.max_heightfield_cells)
      throw std::invalid_argument("render resource limits must be nonzero");
  }
  std::optional<Bounds> mesh_bounds(const ShapeInstance &shape,
                                    const std::shared_ptr<const MeshData> &mesh,
                                    const MeshScale &scale) {
    if (!bounds_cache)
      bounds_cache = std::make_unique<BoundsCache>();
    // Use the high bits after multiplication so aligned wire IDs don't leave
    // most slots unused. Collisions only replace a cached result.
    constexpr std::uint64_t multiplier = 0x9e3779b97f4a7c15ull;
    const auto attachment = shape.actor_id * multiplier + shape.shape_id;
    auto &entry = (*bounds_cache)[(attachment * multiplier) >> (64 - bounds_cache_bits)];
    const auto &pose = shape.world_pose;
    const std::array<float, 14> transform{
        pose.position.x, pose.position.y, pose.position.z,
        pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w,
        scale.scale.x, scale.scale.y, scale.scale.z,
        scale.rotation.x, scale.rotation.y, scale.rotation.z, scale.rotation.w};
    // The immutable mesh owner identifies the decoded version. Compare float
    // representations to preserve signed zero in the exact vertex bounds.
    if (!entry.mesh.owner_before(mesh) && !mesh.owner_before(entry.mesh) &&
        std::memcmp(entry.transform.data(), transform.data(),
                    transform.size() * sizeof(float)) == 0)
      return entry.value;
    auto value = bounds(shape.geometry, pose);
    entry = {mesh, transform, value};
    return value;
  }
  std::optional<Bounds> world_bounds(const ShapeInstance &shape) {
    if (options.max_cache_entries) {
      if (auto mesh = std::get_if<TriangleMesh>(&shape.geometry))
        return mesh_bounds(shape, mesh->mesh, mesh->scale);
      if (auto mesh = std::get_if<ConvexMesh>(&shape.geometry))
        return mesh_bounds(shape, mesh->mesh, mesh->scale);
      if (auto field = std::get_if<HeightField>(&shape.geometry))
        return mesh_bounds(shape, field->mesh,
                           {{field->row_scale, field->height_scale, field->column_scale}, {}});
    }
    return bounds(shape.geometry, shape.world_pose);
  }
  void limit(std::uint64_t n, std::size_t max, const InstanceState &o,
             const std::string &p) {
    if (n > max || n > std::numeric_limits<std::uint32_t>::max())
      fail(DiagnosticCode::resource_limit, o, p,
           "geometry exceeds configured resource limit");
  }
  std::vector<Vec3> points(const InstanceState &o) {
    const auto &p = need(o, "Points");
    if (!type(p, "PxVec3"))
      fail(DiagnosticCode::invalid_type, o, "Points", "expected PxVec3 array");
    size_is(o, "Points", p, 12);
    limit(p.item_count, options.max_vertices, o, "Points");
    std::vector<Vec3> data;
    data.reserve(static_cast<std::size_t>(p.item_count));
    for (std::size_t i = 0; i < p.item_count; ++i) {
      auto v = *p.as_vec3(i);
      if (!finite(v))
        fail(DiagnosticCode::invalid_value, o, "Points",
             "non-finite mesh vertex");
      data.push_back(v);
    }
    if (data.empty())
      fail(DiagnosticCode::count_mismatch, o, "Points", "mesh has no vertices");
    return data;
  }
  std::shared_ptr<const MeshData> mesh(const InstanceState &o,
                                       GeometryKind kind) {
    Signature sig;
    auto input = [&](const char *p, bool required = true) {
      auto v = o.property(p);
      if (v)
        sig.values.emplace_back(p, *v);
      else if (required)
        need(o, p);
    };
    if (kind == GeometryKind::height_field) {
      input("NbRows");
      input("NbColumns");
      input("Format");
      input("Samples");
    } else {
      input("Points");
      if (kind == GeometryKind::triangle_mesh) {
        input("NbTriangles");
        input("Triangles");
        input("MaterialIndices", false);
      } else {
        input("HullPolygons");
        input("PolygonIndexes");
      }
    }
    Key key{o.id, o.generation, kind};
    auto found = cache.find(key);
    if (found != cache.end() && found->second.signature == sig) {
      ++hits;
      found->second.used = ++clock;
      return found->second.mesh;
    }
    const auto live = live_meshes.find(key);
    if (live != live_meshes.end() && live->second.matches(sig))
      if (auto data = live->second.mesh.lock()) {
        ++hits;
        retain(key, sig, data);
        return data;
      }
    ++misses;
    auto data = std::make_shared<MeshData>();
    data->resource = {o.id, o.generation};
    data->kind = kind;
    if (kind == GeometryKind::height_field)
      decode_heightfield(o, *data);
    else {
      data->vertices = points(o);
      if (kind == GeometryKind::triangle_mesh)
        decode_triangles(o, *data);
      else
        decode_convex(o, *data);
    }
    retain(key, sig, data);
    return data;
  }
  void decode_triangles(const InstanceState &o, MeshData &d) {
    auto count = integer(o, "NbTriangles");
    limit(count, options.max_triangles, o, "NbTriangles");
    if (!count)
      fail(DiagnosticCode::count_mismatch, o, "NbTriangles",
           "triangle mesh has no triangles");
    const auto &p = need(o, "Triangles");
    std::size_t width = type(p, "PvdU16") ? 2 : type(p, "PvdU32") ? 4 : 0;
    if (!width)
      fail(DiagnosticCode::invalid_type, o, "Triangles",
           "expected 16- or 32-bit unsigned indices");
    size_is(o, "Triangles", p, width);
    if (p.item_count != count * 3)
      fail(DiagnosticCode::count_mismatch, o, "Triangles",
           "index count must equal 3 * NbTriangles");
    d.indices.reserve(static_cast<std::size_t>(p.item_count));
    for (std::size_t i = 0; i < p.item_count; ++i) {
      auto index = uint_at(o, "Triangles", p, i * width, width);
      if (index >= d.vertices.size())
        fail(DiagnosticCode::invalid_index, o, "Triangles",
             "triangle index exceeds vertex count");
      d.indices.push_back(static_cast<std::uint32_t>(index));
    }
    if (auto m = o.property("MaterialIndices")) {
      if (!type(*m, "PvdU16"))
        fail(DiagnosticCode::invalid_type, o, "MaterialIndices",
             "expected PvdU16");
      size_is(o, "MaterialIndices", *m, 2);
      if (m->item_count != count)
        fail(DiagnosticCode::count_mismatch, o, "MaterialIndices",
             "material count differs from triangles");
      for (std::size_t i = 0; i < m->item_count; ++i)
        d.material_indices.push_back(static_cast<std::uint16_t>(
            uint_at(o, "MaterialIndices", *m, i * 2, 2)));
    }
  }
  void decode_convex(const InstanceState &o, MeshData &d) {
    const auto &p = need(o, "HullPolygons"), &ix = need(o, "PolygonIndexes");
    if (p.incoming_type != QualifiedName{"physx3", "PvdHullPolygonData"})
      fail(DiagnosticCode::invalid_type, o, "HullPolygons",
           "expected PvdHullPolygonData");
    size_is(o, "HullPolygons", p, 4);
    if (!type(ix, "PvdU8"))
      fail(DiagnosticCode::invalid_type, o, "PolygonIndexes",
           "expected byte indices");
    size_is(o, "PolygonIndexes", ix, 1);
    if (!p.item_count)
      fail(DiagnosticCode::count_mismatch, o, "HullPolygons",
           "convex has no polygons");
    std::uint64_t triangles = 0;
    for (std::size_t i = 0; i < p.item_count; ++i) {
      auto n = uint_at(o, "HullPolygons", p, i * 4, 2),
           base = uint_at(o, "HullPolygons", p, i * 4 + 2, 2);
      if (n < 3 || base > ix.item_count || n > ix.item_count - base)
        fail(DiagnosticCode::invalid_index, o, "HullPolygons",
             "polygon range is invalid");
      triangles += n - 2;
      limit(triangles, options.max_triangles, o, "HullPolygons");
      for (std::size_t j = 0; j < n; ++j)
        if (ix.raw_bytes()[base + j] >= d.vertices.size())
          fail(DiagnosticCode::invalid_index, o, "PolygonIndexes",
               "convex polygon index exceeds vertex count");
      auto a = ix.raw_bytes()[base];
      for (std::size_t j = 1; j + 1 < n; ++j) {
        d.indices.push_back(a);
        d.indices.push_back(ix.raw_bytes()[base + j]);
        d.indices.push_back(ix.raw_bytes()[base + j + 1]);
      }
    }
  }
  void decode_heightfield(const InstanceState &o, MeshData &d) {
    auto rows = integer(o, "NbRows"), cols = integer(o, "NbColumns");
    if (rows < 2 || cols < 2 ||
        rows > std::numeric_limits<std::uint32_t>::max() ||
        cols > std::numeric_limits<std::uint32_t>::max())
      fail(DiagnosticCode::invalid_value, o, "NbRows",
           "heightfield requires at least 2 rows and columns within uint32");
    auto cells = rows * cols;
    limit(cells, options.max_heightfield_cells, o, "Samples");
    limit(cells, options.max_vertices, o, "Samples");
    limit((rows - 1) * (cols - 1) * 2, options.max_triangles, o, "Samples");
    if (integer(o, "Format") != 1)
      fail(DiagnosticCode::invalid_value, o, "Format",
           "unsupported heightfield format (expected eS16_TM=1)");
    const auto &p = need(o, "Samples");
    if (p.incoming_type != QualifiedName{"physx3", "PxHeightFieldSample"})
      fail(DiagnosticCode::invalid_type, o, "Samples",
           "expected PxHeightFieldSample");
    size_is(o, "Samples", p, 4);
    if (p.item_count != cells)
      fail(DiagnosticCode::count_mismatch, o, "Samples",
           "heightfield sample count differs from rows * columns");
    d.vertices.reserve(static_cast<std::size_t>(cells));
    for (std::uint64_t r = 0; r < rows; ++r)
      for (std::uint64_t c = 0; c < cols; ++c) {
        auto raw = uint_at(o, "Samples", p,
                           static_cast<std::size_t>((r * cols + c) * 4), 2);
        auto h = raw & 0x8000 ? static_cast<std::int32_t>(raw) - 65536
                              : static_cast<std::int32_t>(raw);
        d.vertices.push_back({float(r), float(h), float(c)});
      }
    auto tri = [&](std::uint8_t material, std::uint64_t a, std::uint64_t b,
                   std::uint64_t c) {
      material &= 127;
      if (material == 127)
        return;
      d.indices.push_back(static_cast<std::uint32_t>(a));
      d.indices.push_back(static_cast<std::uint32_t>(b));
      d.indices.push_back(static_cast<std::uint32_t>(c));
      d.material_indices.push_back(material);
    };
    for (std::uint64_t r = 0; r + 1 < rows; ++r)
      for (std::uint64_t c = 0; c + 1 < cols; ++c) {
        auto a = r * cols + c, b = a + 1, x = a + cols, e = x + 1;
        auto m0 = p.raw_bytes()[a * 4 + 2], m1 = p.raw_bytes()[a * 4 + 3];
        if (m0 & 128) {
          tri(m0, x, a, e);
          tri(m1, b, e, a);
        } else {
          tri(m0, a, b, x);
          tri(m1, e, x, b);
        }
      }
  }

  template <class SnapshotLike>
  Geometry geometry(const Metadata &m, const SnapshotLike &s,
                    const InstanceState &o) {
    if (is_a(m, o, "PxBoxGeometry")) {
      auto e = vector(o, "HalfExtents");
      positive(e.x, o, "HalfExtents");
      positive(e.y, o, "HalfExtents");
      positive(e.z, o, "HalfExtents");
      return Box{e};
    }
    if (is_a(m, o, "PxSphereGeometry")) {
      auto r = number(o, "Radius");
      positive(r, o, "Radius");
      return Sphere{r};
    }
    if (is_a(m, o, "PxCapsuleGeometry")) {
      auto r = number(o, "Radius"), h = number(o, "HalfHeight");
      positive(r, o, "Radius");
      positive(h, o, "HalfHeight");
      return Capsule{r, h};
    }
    if (is_a(m, o, "PxPlaneGeometry"))
      return Plane{};
    if (is_a(m, o, "PxConvexMeshGeometry")) {
      auto id = reference(o, "ConvexMesh");
      const auto &resource = resolve(s, o, "ConvexMesh", id);
      if (!is_a(m, resource, "PxConvexMesh"))
        fail(DiagnosticCode::invalid_type, o, "ConvexMesh",
             "resource is not PxConvexMesh");
      auto scaling = scale(o, true);
      auto flags = integer(o, "MeshFlags");
      if (flags > UINT32_MAX)
        fail(DiagnosticCode::invalid_value, o, "MeshFlags",
             "flags exceed uint32");
      return ConvexMesh{mesh(resource, GeometryKind::convex_mesh), scaling,
                        static_cast<std::uint32_t>(flags)};
    }
    if (is_a(m, o, "PxTriangleMeshGeometry")) {
      auto id = reference(o, "TriangleMesh");
      const auto &resource = resolve(s, o, "TriangleMesh", id);
      if (!is_a(m, resource, "PxTriangleMesh"))
        fail(DiagnosticCode::invalid_type, o, "TriangleMesh",
             "resource is not PxTriangleMesh");
      auto scaling = scale(o, false);
      auto flags = integer(o, "MeshFlags");
      if (flags > UINT32_MAX)
        fail(DiagnosticCode::invalid_value, o, "MeshFlags",
             "flags exceed uint32");
      return TriangleMesh{mesh(resource, GeometryKind::triangle_mesh), scaling,
                          static_cast<std::uint32_t>(flags), (flags & 2) != 0};
    }
    if (is_a(m, o, "PxHeightFieldGeometry")) {
      auto id = reference(o, "HeightField");
      const auto &resource = resolve(s, o, "HeightField", id);
      if (!is_a(m, resource, "PxHeightField"))
        fail(DiagnosticCode::invalid_type, o, "HeightField",
             "resource is not PxHeightField");
      auto h = number(o, "HeightScale"), r = number(o, "RowScale"),
           c = number(o, "ColumnScale");
      positive(h, o, "HeightScale");
      positive(r, o, "RowScale");
      positive(c, o, "ColumnScale");
      // PxHeightFieldGeometry.h SDK validity thresholds, not arbitrary epsilon.
      if (r < 1e-8f || c < 1e-8f || h < (0.0001f / 65535.0f))
        fail(DiagnosticCode::invalid_value, o, "HeightScale",
             "heightfield scale is below the SDK minimum");
      auto flags = integer(o, "HeightFieldFlags");
      if (flags > UINT32_MAX)
        fail(DiagnosticCode::invalid_value, o, "HeightFieldFlags",
             "flags exceed uint32");
      auto data = mesh(resource, GeometryKind::height_field);
      return HeightField{
          data,
          static_cast<std::uint32_t>(integer(resource, "NbRows")),
          static_cast<std::uint32_t>(integer(resource, "NbColumns")),
          h,
          r,
          c,
          static_cast<std::uint32_t>(flags),
          (flags & 2) != 0};
    }
    fail(DiagnosticCode::unsupported_geometry, o, "Geometry",
         "unsupported geometry class " + o.class_name.str());
  }
  template <class SnapshotLike>
  const InstanceState &scene_for_actor(const Metadata &m, const SnapshotLike &s,
                                       const InstanceState &actor,
                                       const InstanceState *&articulation) {
    const auto *current = &actor;
    std::set<std::uint64_t> visited;
    while (true) {
      if (!visited.insert(current->id).second)
        fail(DiagnosticCode::missing_scene, actor, "Parent",
             "cycle in articulation parent chain");
      if (is_a(m, *current, "PxArticulation"))
        articulation = current;
      if (current->property("Scene")) {
        auto id = reference(*current, "Scene");
        if (id) {
          const auto &sc = resolve(s, *current, "Scene", id);
          if (!is_a(m, sc, "PxScene"))
            fail(DiagnosticCode::invalid_type, *current, "Scene",
                 "reference is not PxScene");
          return sc;
        }
      }
      if (!is_a(m, actor, "PxArticulationLink") || !current->property("Parent"))
        fail(DiagnosticCode::missing_scene, actor, "Scene",
             "actor has no live scene ownership");
      current = &resolve(s, *current, "Parent", reference(*current, "Parent"));
    }
  }
  template <class SnapshotLike>
  Frame extract(const Metadata &m, const SnapshotLike &s, const ExtractOptions &o) {
    class_checks.clear();
    Frame result;
    result.event_count = s.event_count;
    if (o.scene_id) {
      auto it = s.origin_shifts.find(*o.scene_id);
      if (it != s.origin_shifts.end())
        result.origin_shifts.emplace(*it);
    } else
      result.origin_shifts = s.origin_shifts;
    auto diagnostic = [&](const Failure &e, std::uint64_t actor,
                          std::uint64_t shape) {
      result.diagnostics.push_back(
          {Severity::error, e.code, e.id, actor, shape, e.property, e.message});
    };
    if (o.scene_id) {
      auto scene = s.instance(*o.scene_id);
      if (!scene || !is_a(m, *scene, "PxScene")) {
        result.diagnostics.push_back(
            {Severity::error, DiagnosticCode::missing_scene, *o.scene_id, 0, 0,
             "Scene", "requested scene is absent or has unsupported type"});
        return result;
      }
    }
    // Cache ownership never outlives a resource lifetime unless a returned
    // Frame still owns the immutable MeshData. Historical seek will decode
    // again.
    prune_live_meshes();
    for (auto it = cache.begin(); it != cache.end();) {
      auto resource = s.instance(std::get<0>(it->first));
      if (!resource || resource->generation != std::get<1>(it->first))
        it = cache.erase(it);
      else
        ++it;
    }
    bool any_actor = false;
    for (const auto &[id, actor] : s.instances) {
      if (!is_a(m, actor, "PxRigidActor") && !is_a(m, actor, "PxRigidStatic") &&
          !is_a(m, actor, "PxRigidDynamic") &&
          !is_a(m, actor, "PxArticulationLink"))
        continue;
      any_actor = true;
      try {
        ActorStatus status;
        if (is_a(m, actor, "PxRigidStatic"))
          status.kind = ActorKind::rigid_static;
        else if (is_a(m, actor, "PxRigidDynamic"))
          status.kind = ActorKind::rigid_dynamic;
        else if (is_a(m, actor, "PxArticulationLink"))
          status.kind = ActorKind::articulation_link;
        else
          fail(DiagnosticCode::unsupported_actor, actor, "Class",
               "unsupported rigid actor class " + actor.class_name.str());
        const InstanceState *articulation = nullptr;
        const auto &scene = scene_for_actor(m, s, actor, articulation);
        if (o.scene_id && scene.id != *o.scene_id)
          continue;
        auto actor_pose = pose(actor, "GlobalPose");
        if (status.kind != ActorKind::rigid_static) {
          auto flags = integer(actor, "RigidBodyFlags");
          if (flags > UINT32_MAX)
            fail(DiagnosticCode::invalid_value, actor, "RigidBodyFlags",
                 "flags exceed uint32");
          status.rigid_body_flags = static_cast<std::uint32_t>(flags);
          status.kinematic = (flags & 1) != 0;
          const InstanceState *sleeper =
              actor.property("IsSleeping") ? &actor : articulation;
          if (sleeper && sleeper->property("IsSleeping")) {
            auto value = integer(*sleeper, "IsSleeping");
            if (value > 1)
              fail(DiagnosticCode::invalid_value, *sleeper, "IsSleeping",
                   "boolean must be zero or one");
            status.sleeping = value != 0;
          } else
            result.diagnostics.push_back(
                {Severity::warning, DiagnosticCode::missing_property, actor.id,
                 actor.id, 0, "IsSleeping", "sleep state is unavailable"});
        }
        auto shape_ids = references(actor, "Shapes");
        std::set<std::uint64_t> seen;
        for (auto shape_id : shape_ids) {
          if (!seen.insert(shape_id).second)
            continue;
          try {
            const auto &shape = resolve(s, actor, "Shapes", shape_id);
            if (!is_a(m, shape, "PxShape"))
              fail(DiagnosticCode::invalid_type, shape, "Class",
                   "actor shape reference is not PxShape");
            auto geometry_id = reference(shape, "Geometry");
            const auto &geometry_object =
                resolve(s, shape, "Geometry", geometry_id);
            ShapeInstance instance;
            instance.actor_id = actor.id;
            instance.actor_generation = actor.generation;
            instance.shape_id = shape.id;
            instance.shape_generation = shape.generation;
            instance.scene_id = scene.id;
            instance.scene_generation = scene.generation;
            instance.geometry_id = geometry_object.id;
            instance.geometry_generation = geometry_object.generation;
            instance.world_pose = compose(actor_pose, pose(shape, "LocalPose"));
            if (!finite(instance.world_pose.position) ||
                !unit(instance.world_pose.rotation))
              fail(DiagnosticCode::invalid_value, shape, "LocalPose",
                   "composed world pose overflowed or lost unit length");
            auto flags = integer(shape, "Flags");
            if (flags > UINT32_MAX)
              fail(DiagnosticCode::invalid_value, shape, "Flags",
                   "flags exceed uint32");
            instance.flags = {static_cast<std::uint32_t>(flags),
                              (flags & 1) != 0, (flags & 2) != 0,
                              (flags & 4) != 0, (flags & 8) != 0};
            if (instance.flags.simulation && instance.flags.trigger)
              fail(DiagnosticCode::invalid_value, shape, "Flags",
                   "simulation and trigger flags cannot both be set");
            instance.actor = status;
            instance.color = shape.color ? shape.color : actor.color;
            instance.pickable = shape.pickable && actor.pickable;
            instance.selected =
                std::find(o.selected.begin(), o.selected.end(),
                          instance.picking_key()) != o.selected.end();
            instance.geometry = geometry(m, s, geometry_object);
            try {
              instance.world_bounds = world_bounds(instance);
            } catch (const std::overflow_error &e) {
              fail(DiagnosticCode::invalid_value, geometry_object, "Bounds",
                   e.what());
            }
            if (auto hf = std::get_if<HeightField>(&instance.geometry);
                hf && hf->mesh->indices.empty())
              result.diagnostics.push_back(
                  {Severity::warning, DiagnosticCode::invalid_value,
                   geometry_object.id, actor.id, shape.id, "Samples",
                   "heightfield contains no non-hole triangles"});
            result.instances.push_back(std::move(instance));
          } catch (const Failure &e) {
            diagnostic(e, actor.id, shape_id);
          }
        }
      } catch (const Failure &e) {
        diagnostic(e, actor.id, 0);
      }
    }
    if (!any_actor && !o.scene_id)
      result.diagnostics.push_back(
          {Severity::warning, DiagnosticCode::unsupported_actor, 0, 0, 0,
           "Class", "snapshot contains no supported rigid actors"});
    return result;
  }
  template <class SnapshotLike>
  GeometryExtraction standalone_geometry(const Metadata &metadata,
      const SnapshotLike &snapshot, ObjectKey requested) {
    class_checks.clear();
    GeometryExtraction result;
    prune_live_meshes();
    result.source = requested;
    const auto *object = snapshot.instance(requested.id);
    if (!object || object->generation != requested.generation) {
      result.diagnostics.push_back({Severity::error, DiagnosticCode::dangling_reference,
          requested.id, 0, 0, "Geometry", "recorded geometry is absent or its generation is stale"});
      return result;
    }
    try {
      result.geometry = geometry(metadata, snapshot, *object);
    } catch (const Failure &failure) {
      result.diagnostics.push_back({Severity::error, failure.code, failure.id,
          0, 0, failure.property, failure.message});
    }
    return result;
  }
};
Extractor::Extractor(ExtractorOptions o) : impl_(std::make_unique<Impl>(o)) {}
Extractor::~Extractor() = default;
Extractor::Extractor(Extractor &&) noexcept = default;
Extractor &Extractor::operator=(Extractor &&) noexcept = default;
Frame Extractor::extract(const Metadata &m, const Snapshot &s,
                         ExtractOptions o) {
  return impl_->extract(m, s, o);
}
Frame Extractor::extract(const Metadata &m, const SnapshotView &s,
                         ExtractOptions o) {
  return impl_->extract(m, s, o);
}
GeometryExtraction Extractor::extract_geometry(const Metadata &metadata,
                                               const Snapshot &snapshot,
                                               ObjectKey requested) {
  return impl_->standalone_geometry(metadata, snapshot, requested);
}
GeometryExtraction Extractor::extract_geometry(const Metadata &metadata,
                                               const SnapshotView &snapshot,
                                               ObjectKey requested) {
  return impl_->standalone_geometry(metadata, snapshot, requested);
}
void Extractor::clear_cache() noexcept {
  impl_->cache.clear();
  impl_->live_meshes.clear();
  impl_->bounds_cache.reset();
  impl_->clock = 0;
  impl_->hits = impl_->misses = 0;
}
CacheStats Extractor::cache_stats() const noexcept {
  return {impl_->cache.size(), impl_->hits, impl_->misses};
}
} // namespace pvd::render
