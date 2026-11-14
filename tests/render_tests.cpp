#include "pvd/render.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace pvd;
using namespace pvd::render;
namespace {
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
void near(float a, float b, const char *m) {
  check(std::isfinite(a) && std::abs(a - b) < 1e-4f, m);
}
void near(Vec3 a, Vec3 b, const char *m) {
  near(a.x, b.x, m);
  near(a.y, b.y, m);
  near(a.z, b.z, m);
}
Bytes integers(std::initializer_list<std::uint64_t> list, std::size_t width,
               ByteOrder order = ByteOrder::little) {
  Bytes out;
  for (auto v : list)
    for (std::size_t i = 0; i < width; ++i)
      out.push_back(static_cast<std::uint8_t>(
          v >> ((order == ByteOrder::little ? i : width - i - 1) * 8)));
  return out;
}
Bytes floats(std::initializer_list<float> list,
             ByteOrder order = ByteOrder::little) {
  Bytes out;
  for (auto n : list) {
    std::uint32_t bits;
    std::memcpy(&bits, &n, 4);
    auto b = integers({bits}, 4, order);
    out.insert(out.end(), b.begin(), b.end());
  }
  return out;
}
PropertyValue value(const char *name, Bytes bytes, std::uint64_t count = 1,
                    ByteOrder order = ByteOrder::little,
                    std::uint8_t pointers = 8) {
  PropertyValue p;
  p.incoming_type = {"physx3", name};
  p.item_count = count;
  p.byte_order = order;
  p.pointer_width = pointers;
  p.bytes = std::make_shared<const Bytes>(std::move(bytes));
  return p;
}
struct Fixture {
  Metadata metadata;
  Snapshot state;
  ByteOrder order = ByteOrder::little;
  std::uint8_t pointers = 8;
  explicit Fixture(ByteOrder o = ByteOrder::little, std::uint8_t ptr = 8)
      : order(o), pointers(ptr) {
    for (const char *n : {"PxScene",
                          "PxActor",
                          "PxRigidActor",
                          "PxRigidBody",
                          "PxRigidStatic",
                          "PxRigidDynamic",
                          "PxShape",
                          "PxGeometry",
                          "PxBoxGeometry",
                          "PxSphereGeometry",
                          "PxCapsuleGeometry",
                          "PxPlaneGeometry",
                          "PxTriangleMeshGeometry",
                          "PxConvexMeshGeometry",
                          "PxHeightFieldGeometry",
                          "PxTriangleMesh",
                          "PxConvexMesh",
                          "PxHeightField",
                          "PxArticulation",
                          "PxArticulationLink"}) {
      QualifiedName type{"physx3", n};
      metadata.classes.emplace(type, ClassDefinition{type, {}, {}});
    }
    metadata.classes.at({"physx3", "PxRigidActor"}).parent =
        QualifiedName{"physx3", "PxActor"};
    metadata.classes.at({"physx3", "PxRigidBody"}).parent =
        QualifiedName{"physx3", "PxRigidActor"};
    metadata.classes.at({"physx3", "PxRigidStatic"}).parent =
        QualifiedName{"physx3", "PxRigidActor"};
    metadata.classes.at({"physx3", "PxRigidDynamic"}).parent =
        QualifiedName{"physx3", "PxRigidBody"};
    metadata.classes.at({"physx3", "PxArticulationLink"}).parent =
        QualifiedName{"physx3", "PxRigidBody"};
    object(1, "PxScene");
    object(2, "PxScene");
    actor(10, 1);
    shape(100, 200);
    refs(10, "Shapes", {100});
    box(200, {1, 2, 3});
    state.event_count = 50;
  }
  InstanceState &object(std::uint64_t id, const char *type) {
    auto &o = state.instances[id];
    o.id = id;
    o.class_name = {"physx3", type};
    return o;
  }
  void actor(std::uint64_t id, std::uint64_t scene, bool dynamic = false) {
    object(id, dynamic ? "PxRigidDynamic" : "PxRigidStatic");
    refs(id, "Scene", {scene});
    transform(id, "GlobalPose", {});
    refs(id, "Shapes", {});
    if (dynamic) {
      u(id, "RigidBodyFlags", 0, 1);
      u(id, "IsSleeping", 0, 1, "PvdBool");
    }
  }
  void shape(std::uint64_t id, std::uint64_t geometry) {
    object(id, "PxShape");
    refs(id, "Geometry", {geometry});
    u(id, "Flags", 11, 1);
    transform(id, "LocalPose", {});
  }
  void refs(std::uint64_t id, const char *name,
            std::initializer_list<std::uint64_t> ids, bool decoded = true) {
    auto p = value("VoidPtr", integers(ids, pointers, order), ids.size(), order,
                   pointers);
    if (decoded)
      p.reference_ids = std::vector<std::uint64_t>(ids);
    state.instances.at(id).properties[name] = std::move(p);
  }
  void u(std::uint64_t id, const char *name, std::uint64_t n,
         std::size_t width = 4, const char *type = nullptr) {
    if (!type)
      type = width == 1   ? "PvdU8"
             : width == 2 ? "PvdU16"
             : width == 4 ? "PvdU32"
                          : "PvdU64";
    state.instances.at(id).properties[name] =
        value(type, integers({n}, width, order), 1, order, pointers);
  }
  void f(std::uint64_t id, const char *name, float n) {
    state.instances.at(id).properties[name] =
        value("PvdF32", floats({n}, order), 1, order, pointers);
  }
  void vec(std::uint64_t id, const char *name, Vec3 v) {
    state.instances.at(id).properties[name] =
        value("PxVec3", floats({v.x, v.y, v.z}, order), 1, order, pointers);
  }
  void quat(std::uint64_t id, const char *name, Quat q) {
    state.instances.at(id).properties[name] = value(
        "PxQuat", floats({q.x, q.y, q.z, q.w}, order), 1, order, pointers);
  }
  void transform(std::uint64_t id, const char *name, Transform t) {
    state.instances.at(id).properties[name] =
        value("PxTransform",
              floats({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
                      t.position.x, t.position.y, t.position.z},
                     order),
              1, order, pointers);
  }
  void box(std::uint64_t id, Vec3 extents) {
    object(id, "PxBoxGeometry");
    vec(id, "HalfExtents", extents);
  }
  void points(std::uint64_t id) {
    state.instances.at(id).properties["Points"] =
        value("PxVec3", floats({0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, order), 4,
              order, pointers);
  }
  void scaling(std::uint64_t id, Vec3 v = {1, 1, 1}, Quat q = {}) {
    vec(id, "Scale.Scale", v);
    quat(id, "Scale.Rotation", q);
    u(id, "MeshFlags", 0, 1);
  }
  void triangle(std::uint64_t geom = 200, std::uint64_t resource = 300) {
    object(geom, "PxTriangleMeshGeometry");
    refs(geom, "TriangleMesh", {resource});
    scaling(geom);
    object(resource, "PxTriangleMesh");
    points(resource);
    u(resource, "NbTriangles", 2);
    state.instances.at(resource).properties["Triangles"] = value(
        "PvdU16", integers({0, 1, 2, 0, 2, 3}, 2, order), 6, order, pointers);
  }
  void convex() {
    object(200, "PxConvexMeshGeometry");
    refs(200, "ConvexMesh", {300});
    scaling(200);
    object(300, "PxConvexMesh");
    points(300);
    state.instances.at(300).properties["HullPolygons"] =
        value("PvdHullPolygonData",
              integers({3, 0, 3, 3, 3, 6, 3, 9}, 2, order), 4, order, pointers);
    state.instances.at(300).properties["PolygonIndexes"] =
        value("PvdU8", Bytes{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3}, 12, order,
              pointers);
  }
  void heightfield(bool tess = true, bool hole = true) {
    object(200, "PxHeightFieldGeometry");
    refs(200, "HeightField", {300});
    f(200, "HeightScale", .5f);
    f(200, "RowScale", 2);
    f(200, "ColumnScale", 3);
    u(200, "HeightFieldFlags", 2, 1);
    object(300, "PxHeightField");
    u(300, "NbRows", 2);
    u(300, "NbColumns", 2);
    u(300, "Format", 1);
    Bytes b;
    for (auto h : {-2, 0, 1, 2}) {
      auto v = integers({static_cast<std::uint16_t>(h)}, 2, order);
      b.insert(b.end(), v.begin(), v.end());
      b.push_back(
          static_cast<std::uint8_t>((b.size() == 2 && tess ? 128 : 0) | 4));
      b.push_back(static_cast<std::uint8_t>(b.size() == 3 && hole ? 127 : 5));
    }
    state.instances.at(300).properties["Samples"] =
        value("PxHeightFieldSample", b, 4, order, pointers);
  }
};
const ShapeInstance &only(const Frame &f) {
  if (!f.diagnostics.empty()) {
    for (const auto &d : f.diagnostics)
      std::cerr << d.object_id << ":" << d.property << " " << d.message << '\n';
  }
  check(f.instances.size() == 1 && f.diagnostics.empty(),
        "expected one valid instance without diagnostics");
  return f.instances.front();
}
bool diagnostic(const Frame &f, DiagnosticCode code) {
  for (const auto &d : f.diagnostics)
    if (d.code == code)
      return true;
  return false;
}
void primitive_transform_shared_scene_state() {
  Fixture f;
  const float h = std::sqrt(.5f);
  f.transform(10, "GlobalPose", {{0, 0, h, h}, {10, 0, 0}});
  f.transform(100, "LocalPose", {{h, 0, 0, h}, {2, 0, 0}});
  f.state.origin_shifts[1] = {100, 200, 300};
  Extractor e;
  auto frame = e.extract(f.metadata, f.state);
  const auto &s = only(frame);
  near(s.world_pose.position, {10, 2, 0}, "parent rotates local translation");
  near(transform_point(s.world_pose, {0, 1, 0}), {10, 2, 1},
       "quaternion composition order");
  near(s.world_bounds->minimum, {7, 1, -2}, "box world minimum");
  near(s.world_bounds->maximum, {13, 3, 2}, "box world maximum");
  check(frame.origin_shifts.at(1).x == 100,
        "origin offset must remain explicit");
  check(s.flags.simulation && s.flags.scene_query && !s.flags.trigger &&
            s.flags.visualization,
        "shape flags");
  auto old_key = s.picking_key();
  f.actor(11, 2, true);
  f.refs(11, "Shapes", {100});
  f.u(11, "RigidBodyFlags", 1, 1);
  f.u(11, "IsSleeping", 1, 1, "PvdBool");
  f.state.instances.at(11).color = Color{1, 2, 3, 4};
  f.state.instances.at(11).pickable = false;
  auto shared = e.extract(f.metadata, f.state);
  check(shared.instances.size() == 2,
        "shared shape must instantiate once per actor");
  check(shared.instances[0].shape_id == shared.instances[1].shape_id &&
            !(shared.instances[0].picking_key() ==
              shared.instances[1].picking_key()),
        "shared shape picking key must include actor");
  auto &dynamic = shared.instances[1];
  check(dynamic.actor.kinematic && dynamic.actor.sleeping == true &&
            !dynamic.pickable && dynamic.color->a == 4,
        "dynamic status and optional color");
  ExtractOptions opts;
  opts.scene_id = 1;
  opts.selected = {old_key};
  auto filtered = e.extract(f.metadata, f.state, opts);
  check(filtered.instances.size() == 1 && filtered.instances[0].selected &&
            filtered.instances[0].actor_id == 10,
        "scene filtering and stable selection");
  f.object(200, "PxSphereGeometry");
  f.f(200, "Radius", 2);
  auto sphere = e.extract(f.metadata, f.state, opts);
  near(only(sphere).world_bounds->minimum, {8, 0, -2}, "sphere analytic bound");
  f.object(200, "PxCapsuleGeometry");
  f.f(200, "Radius", .5f);
  f.f(200, "HalfHeight", 2);
  auto cap = e.extract(f.metadata, f.state, opts);
  near(only(cap).world_bounds->maximum, {10.5f, 4.5f, .5f},
       "capsule uses local X axis");
  f.object(200, "PxPlaneGeometry");
  auto plane = e.extract(f.metadata, f.state, opts);
  check(!only(plane).world_bounds &&
            std::holds_alternative<Plane>(plane.instances[0].geometry),
        "infinite local X plane");
}
void mesh_scale_cache_and_lifetime() {
  const float angle = .3926990817f;
  MeshScale rotated{{2, 1, 3}, {0, 0, std::sin(angle), std::cos(angle)}};
  near(apply_mesh_scale(rotated, {1, 0, 0}), {1.5f, -.5f, 0},
       "R transpose diag(s) R");
  rotated.scale.x = -2;
  check(rotated.flips_winding(), "negative determinant");
  near(apply_mesh_scale(rotated, {1, 0, 0}), {-.5f, 1.5f, 0},
       "reflected rotated scale");
  Fixture f;
  f.triangle();
  f.scaling(200, {-2, 1, 3}, rotated.rotation);
  Extractor e;
  auto first = e.extract(f.metadata, f.state);
  auto original = std::get<TriangleMesh>(only(first).geometry).mesh;
  check(original->indices == std::vector<std::uint32_t>({0, 1, 2, 0, 2, 3}),
        "triangle16 topology");
  check(
      std::get<TriangleMesh>(first.instances[0].geometry).scale.flips_winding(),
      "instance reflection flag");
  f.transform(10, "GlobalPose", {{}, {10, 20, 30}});
  auto moved = e.extract(f.metadata, f.state);
  check(std::get<TriangleMesh>(only(moved).geometry).mesh == original &&
            e.cache_stats().hits > 0,
        "pose-only changes reuse immutable mesh");
  auto history = f.state;
  f.state.instances.at(300).properties["Points"] =
      value("PxVec3", floats({0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1}), 4);
  auto changed = e.extract(f.metadata, f.state);
  auto changed_mesh = std::get<TriangleMesh>(only(changed).geometry).mesh;
  check(changed_mesh != original && changed_mesh->vertices[1].x == 2 &&
            original->vertices[1].x == 1,
        "updated vertices must invalidate cache without mutating old frames");
  f.state.instances.at(300).generation = 1;
  auto reused = e.extract(f.metadata, f.state);
  check(std::get<TriangleMesh>(only(reused).geometry).mesh != changed_mesh &&
            e.cache_stats().entries == 1,
        "resource lifetime reuse invalidates geometry");
  auto old = e.extract(f.metadata, history);
  check(std::get<TriangleMesh>(only(old).geometry).mesh->vertices[1].x == 1,
        "backward seek uses historic resource bytes");
  f.state.instances.at(300).properties["Triangles"] =
      value("PvdU16", integers({999, 1, 2, 0, 2, 3}, 2), 6);
  auto invalid = e.extract(f.metadata, f.state);
  check(invalid.instances.empty() &&
            diagnostic(invalid, DiagnosticCode::invalid_index),
        "malformed new indices cannot return stale cached mesh");
  e.clear_cache();
  check(e.cache_stats().entries == 0 && original->vertices[1].x == 1,
        "cache clear preserves returned frame resources");
  Fixture convex;
  convex.convex();
  auto cv = e.extract(convex.metadata, convex.state);
  const auto &cm = std::get<ConvexMesh>(only(cv).geometry);
  check(cm.mesh->indices.size() == 12 && cm.mesh->vertices.size() == 4,
        "convex polygon fan");
  convex.vec(200, "Scale.Scale", {-1, 1, 1});
  auto badconvex = e.extract(convex.metadata, convex.state);
  check(badconvex.instances.empty() &&
            diagnostic(badconvex, DiagnosticCode::invalid_value),
        "convex negative scale rejected");
  ExtractorOptions budget;
  budget.max_cache_entries = 1;
  Extractor bounded(budget);
  Fixture many;
  many.triangle();
  many.triangle(201, 301);
  many.shape(101, 201);
  many.refs(10, "Shapes", {100, 101});
  auto all = bounded.extract(many.metadata, many.state);
  check(all.instances.size() == 2 && bounded.cache_stats().entries == 1,
        "bounded cache eviction");
  check(
      std::get<TriangleMesh>(all.instances[0].geometry).mesh->indices.size() ==
          6,
      "eviction preserves frame-owned resources");
}
void heightfield_endian_and_diagnostics() {
  Fixture f(ByteOrder::big, 4);
  f.heightfield();
  f.refs(10, "Scene", {1}, false);
  f.refs(10, "Shapes", {100}, false);
  f.refs(100, "Geometry", {200}, false);
  f.refs(200, "HeightField", {300}, false);
  Extractor e;
  auto output = e.extract(f.metadata, f.state);
  auto &g = std::get<HeightField>(only(output).geometry);
  check(g.mesh->vertices[0].y == -2 &&
            g.mesh->indices == std::vector<std::uint32_t>({2, 0, 3}) &&
            g.mesh->material_indices == std::vector<std::uint16_t>({4}),
        "signed big-endian height/tess/hole");
  near(only(output).world_bounds->minimum, {0, -1, 0},
       "heightfield scaled minimum");
  near(only(output).world_bounds->maximum, {2, 1, 3},
       "heightfield scaled maximum");
  f.heightfield(false, false);
  auto alternate = e.extract(f.metadata, f.state);
  check(std::get<HeightField>(only(alternate).geometry).mesh->indices ==
            std::vector<std::uint32_t>({0, 1, 2, 3, 2, 1}),
        "heightfield other tessellation");
  Fixture tri(ByteOrder::big, 4);
  tri.triangle();
  tri.refs(10, "Scene", {1}, false);
  auto triangle = e.extract(tri.metadata, tri.state);
  check(std::get<TriangleMesh>(only(triangle).geometry).mesh->indices[5] == 3,
        "big-endian U16 triangle indices");
  Fixture bad;
  bad.triangle();
  bad.state.instances.at(300).properties["Points"].item_count = UINT64_MAX;
  auto huge = e.extract(bad.metadata, bad.state);
  check(huge.instances.empty() &&
            diagnostic(huge, DiagnosticCode::malformed_payload),
        "overflowing item count rejected");
  bad = Fixture{};
  bad.object(200, "NewGeometry");
  auto unknown = e.extract(bad.metadata, bad.state);
  check(unknown.instances.empty() &&
            diagnostic(unknown, DiagnosticCode::unsupported_geometry),
        "unknown geometry diagnostic");
  bad = Fixture{};
  bad.state.instances.at(100).properties.erase("LocalPose");
  auto missing = e.extract(bad.metadata, bad.state);
  check(missing.instances.empty() &&
            diagnostic(missing, DiagnosticCode::missing_property),
        "missing pose diagnostic");
  bad = Fixture{};
  bad.vec(200, "HalfExtents", {std::numeric_limits<float>::quiet_NaN(), 1, 1});
  auto nan = e.extract(bad.metadata, bad.state);
  check(nan.instances.empty() && diagnostic(nan, DiagnosticCode::invalid_value),
        "NaN rejected");
  bad = Fixture{};
  bad.refs(10, "Shapes", {999});
  auto dangling = e.extract(bad.metadata, bad.state);
  check(dangling.instances.empty() &&
            diagnostic(dangling, DiagnosticCode::dangling_reference),
        "dangling shape diagnostic");
  bad = Fixture{};
  bad.u(100, "Flags", 5, 1);
  auto flags = e.extract(bad.metadata, bad.state);
  check(flags.instances.empty() &&
            diagnostic(flags, DiagnosticCode::invalid_value),
        "contradictory shape flags rejected");
  bad = Fixture{};
  bad.transform(100, "LocalPose", {{0, 0, 0, 0}, {}});
  auto rotation = e.extract(bad.metadata, bad.state);
  check(rotation.instances.empty() &&
            diagnostic(rotation, DiagnosticCode::invalid_value),
        "non-unit quaternion rejected");
  bad = Fixture{};
  bad.triangle();
  ExtractorOptions limits;
  limits.max_vertices = 2;
  Extractor limited(limits);
  auto cap = limited.extract(bad.metadata, bad.state);
  check(cap.instances.empty() &&
            diagnostic(cap, DiagnosticCode::resource_limit),
        "resource vertex budget diagnostic");
  auto filtered = e.extract(bad.metadata, bad.state, ExtractOptions{999, {}});
  check(filtered.instances.empty() &&
            diagnostic(filtered, DiagnosticCode::missing_scene),
        "missing filtered scene diagnostic");
  bad = Fixture{};
  bad.object(200, "PxCapsuleGeometry");
  bad.f(200, "Radius", 1);
  bad.f(200, "HalfHeight", 0);
  auto zero_capsule = e.extract(bad.metadata, bad.state);
  check(zero_capsule.instances.empty() &&
            diagnostic(zero_capsule, DiagnosticCode::invalid_value),
        "SDK capsule requires positive half-height");
  bad = Fixture{};
  bad.heightfield();
  bad.f(200, "RowScale", 1e-10f);
  auto small_hf = e.extract(bad.metadata, bad.state);
  check(small_hf.instances.empty() &&
            diagnostic(small_hf, DiagnosticCode::invalid_value),
        "SDK heightfield minimum scale");
  // A failed shape must not prevent a later valid shape from being extracted.
  bad = Fixture{};
  bad.shape(101, 201);
  bad.object(201, "PxSphereGeometry");
  bad.f(201, "Radius", 1);
  bad.refs(10, "Shapes", {100, 101});
  bad.state.instances.at(100).properties.erase("LocalPose");
  auto partial = e.extract(bad.metadata, bad.state);
  check(partial.instances.size() == 1 && partial.instances[0].shape_id == 101 &&
            partial.diagnostics.size() == 1,
        "per-instance failure isolation");
}
void geometry_without_actor_ownership() {
  for (auto order : {ByteOrder::little, ByteOrder::big}) {
    Fixture f(order);
    f.convex();
    for (auto id : {1u, 2u, 10u, 100u}) f.state.instances.erase(id);
    f.state.instances.at(200).generation = 7;
    Extractor extractor;
    auto geometry = extractor.extract_geometry(f.metadata, f.state, {200, 7});
    check(geometry.geometry && geometry.diagnostics.empty(),
          "recorded standalone convex geometry needs no fake actor/shape");
    const auto mesh = std::get<ConvexMesh>(*geometry.geometry).mesh;
    check(mesh->vertices.size() == 4 && mesh->indices.size() == 12,
          "standalone geometry lost validated hull data");
    auto again = extractor.extract_geometry(f.metadata, f.state, {200, 7});
    check(std::get<ConvexMesh>(*again.geometry).mesh == mesh && extractor.cache_stats().hits > 0,
          "standalone geometry bypassed shared resource cache");
    auto stale = extractor.extract_geometry(f.metadata, f.state, {200, 6});
    check(!stale.geometry && stale.diagnostics.size() == 1 &&
              stale.diagnostics[0].actor_id == 0 && stale.diagnostics[0].shape_id == 0,
          "stale standalone geometry fabricated attachment identity");
    f.state.instances.erase(300);
    check(!extractor.extract_geometry(f.metadata, f.state, {200, 7}).geometry,
          "removed standalone dependency returned stale cached geometry");
    check(mesh->vertices.size() == 4, "retained standalone mesh mutated");
    f.box(200, {1, 2, 3});
    check(std::holds_alternative<Box>(*extractor.extract_geometry(f.metadata, f.state, {200, 7}).geometry),
          "standalone primitive decode failed");
  }
}
} // namespace
int main() {
  try {
    primitive_transform_shared_scene_state();
    mesh_scale_cache_and_lifetime();
    heightfield_endian_and_diagnostics();
    geometry_without_actor_ownership();
    std::cout << "render tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
