#include "model.hpp"
#include "scene_query_fixture.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
using namespace pvd;
using namespace pvd::viewer;
namespace {
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
void near(double a, double b, const char *m) {
  check(std::isfinite(a) && std::abs(a - b) < 1e-4, m);
}
void near(Vec3 a, Vec3 b, const char *m) {
  near(a.x, b.x, m);
  near(a.y, b.y, m);
  near(a.z, b.z, m);
}
Bytes bytes(std::initializer_list<float> list) {
  Bytes out;
  for (float n : list) {
    std::uint32_t x;
    std::memcpy(&x, &n, 4);
    for (int i = 0; i < 4; ++i)
      out.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
  }
  return out;
}
Bytes ids(std::initializer_list<std::uint64_t> list) {
  Bytes out;
  for (auto n : list)
    for (int i = 0; i < 8; ++i)
      out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
  return out;
}
struct Fixture {
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  template <class T> void emit(T value) {
    events.push_back(
        {1, events.size(), events.size() * 9, Payload{std::move(value)}});
  }
  std::uint32_t str(const std::string &s) {
    if (s.empty())
      return 0;
    auto i = strings.find(s);
    if (i != strings.end())
      return i->second;
    auto id = static_cast<std::uint32_t>(strings.size() + 1);
    strings.emplace(s, id);
    emit(StringHandleEvent{s, id});
    return id;
  }
  WireName type(const char *s) {
    auto ns = str("physx3"), name = str(s);
    return {ns, name};
  }
  void prop(const char *c, const char *n, const char *t, bool array = false) {
    auto cls = type(c), dt = type(t);
    auto h = str(n);
    emit(CreateProperty{
        cls, h, 0, dt, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void instance(const char *c, std::uint64_t id) {
    emit(CreateInstance{type(c), id});
  }
  void value(std::uint64_t id, const char *n, const char *t, Bytes b,
             std::uint32_t count = 1) {
    auto h = str(n);
    auto dt = type(t);
    emit(SetPropertyValue{id, h, dt, count, std::move(b)});
  }
  void pose(std::uint64_t id, const char *n, Vec3 p) {
    value(id, n, "PxTransform", bytes({0, 0, 0, 1, p.x, p.y, p.z}));
  }
  void ref(std::uint64_t id, const char *n,
           std::initializer_list<std::uint64_t> list) {
    value(id, n, "VoidPtr", ids(list), static_cast<std::uint32_t>(list.size()));
  }
  void name(std::uint64_t id, const char *n) {
    Bytes b(8, 0);
    b.insert(b.end(), n, n + std::strlen(n) + 1);
    value(id, "Name", "String", std::move(b));
  }
  void begin(std::uint64_t scene, const char *n = "frame") {
    auto h = str(n);
    emit(BeginSection{scene, h, events.size()});
  }
  void end(std::uint64_t scene, const char *n = "frame") {
    auto h = str(n);
    emit(EndSection{scene, h, events.size()});
  }
  Fixture() {
    for (const char *n : {"PxScene", "PxRigidStatic", "PxShape",
                          "PxBoxGeometry", "PxSphereGeometry"})
      emit(CreateClass{type(n)});
    prop("PxScene", "Name", "StringHandle");
    prop("PxRigidStatic", "Scene", "ObjectRef");
    prop("PxRigidStatic", "Shapes", "ObjectRef", true);
    prop("PxRigidStatic", "GlobalPose", "PxTransform");
    prop("PxRigidStatic", "Name", "StringHandle");
    prop("PxShape", "Geometry", "ObjectRef");
    prop("PxShape", "LocalPose", "PxTransform");
    prop("PxShape", "Flags", "PvdU8");
    prop("PxShape", "Name", "StringHandle");
    prop("PxBoxGeometry", "HalfExtents", "PxVec3");
    prop("PxSphereGeometry", "Radius", "PvdF32");
    instance("PxScene", 1);
    instance("PxScene", 2);
    name(1, "first scene");
    name(2, "second scene");
    instance("PxBoxGeometry", 200);
    value(200, "HalfExtents", "PxVec3", bytes({.5f, .5f, .5f}));
    instance("PxSphereGeometry", 201);
    value(201, "Radius", "PvdF32", bytes({1}));
    instance("PxShape", 100);
    ref(100, "Geometry", {200});
    pose(100, "LocalPose", {});
    value(100, "Flags", "PvdU8", Bytes{11});
    name(100, "shared box shape");
    instance("PxShape", 101);
    ref(101, "Geometry", {201});
    pose(101, "LocalPose", {});
    value(101, "Flags", "PvdU8", Bytes{11});
    name(101, "sphere shape");
    actor(10, 1, {0, 0, 0}, 100);
    actor(11, 1, {0, 0, -4}, 100);
    actor(20, 2, {10, 0, 0}, 101);
    begin(2);
    end(2);
    begin(1);
    end(1);
    begin(1, "validation_after_fetch");
    pose(10, "GlobalPose", {1, 0, 0});
    end(1, "validation_after_fetch");
    begin(1);
    pose(10, "GlobalPose", {2, 0, 0});
    emit(DestroyInstance{11});
    end(1);
    begin(1, "validation_after_fetch");
    pose(10, "GlobalPose", {2.5f, 0, 0});
    end(1, "validation_after_fetch");
    begin(1);
    actor(11, 1, {0, 0, -4}, 100);
    pose(10, "GlobalPose", {3, 0, 0});
    end(1);
    begin(1, "validation_after_fetch");
    pose(10, "GlobalPose", {3.5f, 0, 0});
    end(1, "validation_after_fetch");
  }
  void actor(std::uint64_t id, std::uint64_t scene, Vec3 p,
             std::uint64_t shape) {
    instance("PxRigidStatic", id);
    ref(id, "Scene", {scene});
    ref(id, "Shapes", {shape});
    pose(id, "GlobalPose", p);
    name(id, id == 10   ? "first actor"
             : id == 11 ? "second actor"
                        : "other scene actor");
  }
  std::unique_ptr<Session> session() {
    StreamHeader h;
    h.stream_flags = 1;
    return std::make_unique<Session>(h, events);
  }
};
std::array<double, 4> apply(const Mat4 &m, Vec3 p) {
  std::array<double, 4> o{};
  for (int r = 0; r < 4; ++r)
    o[r] = double(m.values[r]) * p.x + double(m.values[4 + r]) * p.y +
           double(m.values[8 + r]) * p.z + m.values[12 + r];
  return o;
}
void camera_and_picking() {
  OrbitCamera c;
  c.yaw = 0;
  c.pitch = 0;
  c.distance = 5;
  c.target = {0, 0, 0};
  near(c.position(), {0, 0, 5}, "RH camera eye");
  auto center = c.screen_ray(400, 300, 800, 600);
  near(center.direction, {0, 0, -1}, "center ray direction");
  auto right = c.screen_ray(800, 300, 800, 600);
  check(right.direction.x > 0 && right.direction.z < 0,
        "screen ray x convention");
  auto top = c.screen_ray(400, 0, 800, 600);
  check(top.direction.y > 0, "screen ray screen y convention");
  auto view = apply(c.view_matrix(), {0, 0, 0});
  near(view[2], -5, "RH view target depth");
  auto n = apply(c.projection_matrix(1), {0, 0, float(-c.near_plane)}),
       f = apply(c.projection_matrix(1), {0, 0, float(-c.far_plane)});
  near(n[2] / n[3], 0, "D3D near depth zero");
  near(f[2] / f[3], 1, "D3D far depth one");
  render::Bounds b{{-2, -1, -3}, {4, 5, 3}};
  c.fit(b, 1.7);
  auto vp = c.view_projection(1.7);
  for (float x : {b.minimum.x, b.maximum.x})
    for (float y : {b.minimum.y, b.maximum.y})
      for (float z : {b.minimum.z, b.maximum.z}) {
        auto p = apply(vp, {x, y, z});
        check(p[3] > 0 && std::abs(p[0] / p[3]) <= 1 &&
                  std::abs(p[1] / p[3]) <= 1 && p[2] / p[3] >= 0 &&
                  p[2] / p[3] <= 1,
              "fit must enclose every bounds corner");
      }
  auto before = c.distance;
  c.zoom(1);
  check(c.distance < before && c.distance > 0, "zoom forward");
  c.orbit(100, 100);
  check(std::isfinite(c.position().y) && std::abs(c.pitch) < 1.5707964,
        "orbit pole clamp");
  auto target = c.target;
  c.pan(20, -10, 600);
  check(c.target.x != target.x || c.target.y != target.y ||
            c.target.z != target.z,
        "pan changes target");
  auto triangle = std::make_shared<render::TriangleBuffer>();
  triangle->positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
  triangle->indices = {0, 1, 2};
  DrawItem first{triangle,         {},   {{1, 0}, {10, 0}, {100, 0}},
                 {255, 0, 0, 255}, true, false};
  auto second = first;
  second.key.actor.id = 11;
  second.world_pose.position.z = -3;
  auto hit = pick({second, first}, {{0, 0, 5}, {0, 0, -2}});
  check(hit && hit->key == first.key,
        "nearest CPU triangle pick across shared shapes");
  near(hit->distance, 5, "ray normalized world distance");
  first.pickable = false;
  hit = pick({first, second}, {{0, 0, 5}, {0, 0, -1}});
  check(hit && hit->key.actor.id == 11, "ignore nonpickable item");
  near(hit->distance, 8, "second shared shape distance");
  first.pickable = true;
  const float h = std::sqrt(.5f);
  first.world_pose.rotation = {0, h, 0, h};
  hit = pick({first}, {{5, 0, 0}, {-1, 0, 0}});
  check(hit && hit->key == first.key, "CPU picking rigid local transform");
  auto resource = std::make_shared<render::MeshData>();
  resource->vertices = triangle->positions;
  resource->indices = triangle->indices;
  render::TriangleMesh scaled{resource, {{2, 1, 1}, {}}, 0, false};
  first.world_pose = {};
  first.mesh = std::make_shared<const render::TriangleBuffer>(
      render::tessellate(scaled));
  hit = pick({first}, {{1.5f, -.75f, 5}, {0, 0, -1}});
  check(bool(hit), "CPU picking uses scale-baked triangles");
  render::MeshBuildOptions options;
  options.plane_half_extent = 2;
  first.mesh = std::make_shared<const render::TriangleBuffer>(
      render::tessellate(render::Plane{}, options));
  check(bool(pick({first}, {{5, 0, 0}, {-1, 0, 0}})),
        "finite plane preview pick");
  check(!pick({first}, {{5, 3, 0}, {-1, 0, 0}}),
        "plane preview does not pick outside explicit extent");
  check(!pick({first}, {{0, 0, 0}, {0, 0, 0}}),
        "invalid zero ray does not pick");
}
void tight_fit_and_zoom_clipping() {
  // Broad, shallow scenes expose the bounding-sphere fit's wasted viewport
  // area.
  for (const auto &bounds : {render::Bounds{{-40, -1, -4}, {40, 1, 4}},
                             render::Bounds{{-2, -30, -1}, {2, 30, 1}},
                             render::Bounds{{10, -2, -8}, {110, 3, 8}}}) {
    for (double aspect : {0.7, 1.7777777777777777, 2.4}) {
      OrbitCamera camera;
      camera.yaw = .75;
      camera.pitch = .45;
      const auto yaw = camera.yaw, pitch = camera.pitch;
      camera.fit(bounds, aspect);
      check(camera.yaw == yaw && camera.pitch == pitch,
            "fit must preserve camera orientation");
      const auto vp = camera.view_projection(aspect);
      double maximum_ndc = 0;
      for (float x : {bounds.minimum.x, bounds.maximum.x})
        for (float y : {bounds.minimum.y, bounds.maximum.y})
          for (float z : {bounds.minimum.z, bounds.maximum.z}) {
            auto p = apply(vp, {x, y, z});
            check(p[3] > 0 && p[2] / p[3] >= -1e-6 && p[2] / p[3] <= 1.000001,
                  "tight fit clips a bounds corner");
            maximum_ndc = std::max(
                {maximum_ndc, std::abs(p[0] / p[3]), std::abs(p[1] / p[3])});
          }
      check(maximum_ndc > .87 && maximum_ndc < .91,
            "fit must use the viewport with the requested lateral margin");
      // Frustum depth remains conservative for orientations other than the one
      // used to choose the tight distance.
      camera.orbit(1.1, -.7);
      auto view = camera.view_matrix();
      for (float x : {bounds.minimum.x, bounds.maximum.x})
        for (float y : {bounds.minimum.y, bounds.maximum.y})
          for (float z : {bounds.minimum.z, bounds.maximum.z}) {
            auto p = apply(view, {x, y, z});
            check(-p[2] < camera.far_plane, "orbit exceeded fitted far range");
          }
      // Zoom toward/through a selected object's bounds without retaining the
      // old fit's large near plane. Its center always remains projectable.
      for (int i = 0; i < 25; ++i) {
        camera.zoom(1);
        check(camera.near_plane > 0 && camera.near_plane < camera.distance &&
                  camera.distance < camera.far_plane,
              "zoom left target outside clipping interval");
        auto p = apply(camera.view_projection(aspect), camera.target);
        check(p[3] > 0 && p[2] / p[3] >= -1e-5 && p[2] / p[3] <= 1.00001,
              "zoom clipped the fitted target");
      }
      camera.zoom(-100);
      auto p = apply(camera.view_projection(aspect), camera.target);
      check(camera.near_plane < camera.distance &&
                camera.distance < camera.far_plane && p[3] > 0 &&
                p[2] / p[3] >= -1e-5 && p[2] / p[3] <= 1.00001,
            "zoom out clipping interval is invalid");
    }
  }
  OrbitCamera flat;
  flat.yaw = 0;
  flat.pitch = 0;
  render::Bounds wide{{-40, -1, -4}, {40, 1, 4}};
  flat.fit(wide, 16.0 / 9);
  const double old_sphere_distance =
      std::sqrt(40.0 * 40 + 1 + 16) / std::sin(flat.vertical_fov / 2) * 1.15;
  check(flat.distance < old_sphere_distance * .7,
        "wide scene fit is still as conservative as its old bounding sphere");
  OrbitCamera point;
  point.fit({{1, 2, 3}, {1, 2, 3}}, 1);
  check(point.near_plane > 0 && point.near_plane < point.distance &&
            point.far_plane > point.distance,
        "degenerate bounds need a usable depth interval");
}
void document_navigation() {
  Document empty;
  check(!empty.loaded() && empty.frame() && empty.frame()->draws.empty(),
        "empty model has stable empty frame");
  Fixture fixture;
  ModelOptions options;
  options.max_tessellation_cache_entries = 2;
  Document doc(options);
  doc.adopt(fixture.session(), "synthetic");
  check(doc.loaded() && doc.scenes().size() == 2 && doc.scene_id() == 1 &&
            doc.timeline().size() == 3,
        "per-scene native timeline");
  auto frame = doc.frame();
  check(frame->draws.size() == 2 && frame->errors.empty() &&
            frame->render.diagnostics.empty(),
        "synthetic shared-shape extraction");
  check(frame->draws[0].mesh == frame->draws[1].mesh,
        "shared analytic geometry tessellates once");
  auto stable_mesh = frame->draws[0].mesh;
  auto key = frame->draws[1].key;
  check(doc.select(key) && doc.inspected_instance()->id == 100,
        "viewport selection inspects shape");
  check(doc.inspect(11) && doc.selected_key() == key &&
            doc.inspected_instance()->id == 11,
        "actor role navigation retains chosen shared attachment");
  check(doc.inspect(1) && doc.selected_key() == key &&
            doc.inspected_instance()->id == 1,
        "scene role navigation retains viewport context");
  check(doc.set_frame(1) && !doc.selected_key(),
        "destroyed attachment clears selection");
  check(frame->draws.size() == 2 && frame->draws[1].key.actor.generation == 0,
        "published old frame remains valid");
  check(doc.frame()->draws[0].mesh == stable_mesh,
        "seek does not rebuild unchanged geometry");
  check(doc.set_frame(0) && doc.select(key) && doc.set_frame(2) &&
            !doc.selected_key(),
        "reused actor ID generation invalidates old picking key");
  check(doc.inspect(200) &&
            doc.inspected_instance()->class_name.name == "PxBoxGeometry" &&
            doc.selected_key(),
        "reference navigation into non-shape geometry");
  check(!doc.inspect(999) && doc.inspected_instance()->id == 200,
        "invalid inspect leaves existing selection");
  auto tree = doc.actor_tree();
  check(tree.size() == 2 && tree[0].shapes.size() == 1 &&
            tree[0].shapes[0].geometry_id == 200,
        "actor/shape tree matches snapshot");
  check(!doc.set_frame(99) && doc.frame_index() == 2,
        "out-of-range seek rejected");
  check(doc.step(std::numeric_limits<std::ptrdiff_t>::min()) &&
            doc.frame_index() == 0,
        "large reverse step clamps");
  check(doc.step(std::numeric_limits<std::ptrdiff_t>::max()) &&
            doc.frame_index() == 2,
        "large forward step clamps");
  check(doc.set_sample_point(SamplePoint::after_fetch) &&
            doc.timeline().size() == 3,
        "explicit post-fetch timeline");
  near(doc.frame()->render.instances[0].world_pose.position.x, 1,
       "post-fetch sample differs from native EndSection");
  check(doc.frame()->label.find("explicit") != std::string::npos,
        "post-fetch label makes semantics clear");
  check(doc.set_sample_point(SamplePoint::native_frame), "native mode switch");
  near(doc.frame()->render.instances[0].world_pose.position.x, 0,
       "native snapshot preserved");
  doc.set_playback({true, false, 2});
  check(!doc.advance(.2) && doc.frame_index() == 0, "fractional playback hold");
  check(doc.advance(.3) && doc.frame_index() == 1,
        "fractional playback advances one frame");
  check(doc.advance(50) && doc.frame_index() == 2 && !doc.playback().playing,
        "playback stops at final frame");
  doc.set_playback({true, true, 2});
  check(doc.advance(.5) && doc.frame_index() == 0, "loop wraps");
  doc.set_playback({true, true, -2});
  check(doc.advance(.5) && doc.frame_index() == 2, "reverse loop wraps");
  OrbitCamera c;
  check(doc.fit_scene(c, 1.3) && doc.fit_selected(c, 1.3),
        "scene and selected fitting");
  check(doc.set_sample_point(SamplePoint::after_fetch) && doc.set_scene(2) &&
            doc.sample_point() == SamplePoint::native_frame &&
            doc.timeline().size() == 1,
        "scene with no post-fetch marker returns to native timeline");
  check(doc.frame()->draws.size() == 1 &&
            doc.frame()->draws[0].key.scene.id == 2,
        "scene filter draws only selected scene");
  check(!doc.set_sample_point(SamplePoint::after_fetch),
        "unsupported post-fetch mode rejected");
  check(!doc.set_scene(999) && doc.scene_id() == 2, "invalid scene rejected");
  check(doc.tessellation_cache_entries() <= 2, "tessellation cache budget");
  auto old = doc.frame();
  bool failed = false;
  try {
    doc.open_file("this-capture-does-not-exist.pxd2");
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed && doc.frame() == old && doc.scene_id() == 2,
        "failed open preserves current document");
}
void active_mesh_reuse() {
  Fixture f;
  for (unsigned i = 0; i < 4; ++i) {
    f.instance("PxBoxGeometry", 1000 + i);
    f.value(1000 + i, "HalfExtents", "PxVec3", bytes({float(i + 1), 2, 3}));
    f.instance("PxShape", 2000 + i);
    f.ref(2000 + i, "Geometry", {1000 + i});
    f.pose(2000 + i, "LocalPose", {});
    f.value(2000 + i, "Flags", "PvdU8", Bytes{11});
    f.actor(3000 + i, 1, {float(i * 10), 0, 0}, 2000 + i);
    f.actor(4000 + i, 1, {float(i * 10), 0, 10}, 2000 + i);
  }
  f.begin(1); f.end(1);
  f.pose(10, "GlobalPose", {9, 0, 0}); f.begin(1); f.end(1);
  ModelOptions options; options.max_tessellation_cache_entries = 1;
  Document doc(options); doc.adopt(f.session()); doc.set_frame(3);
  const auto first = doc.frame();
  std::map<std::uint64_t, std::shared_ptr<const render::TriangleBuffer>> meshes;
  for (const auto &draw : first->draws) meshes[draw.key.actor.id] = draw.mesh;
  for (unsigned i = 0; i < 4; ++i)
    check(meshes.at(3000 + i) == meshes.at(4000 + i),
          "attachments share tessellation with working set larger than LRU");
  check(doc.set_frame(4), "pose-only next sample");
  for (const auto &draw : doc.frame()->draws)
    check(draw.mesh == meshes.at(draw.key.actor.id),
          "pose-only playback preserves all active buffer identities beyond LRU");
  check(doc.tessellation_cache_entries() == 1, "weak reuse does not enlarge strong cache");
  check(doc.set_frame(0) && doc.set_frame(3), "reverse then restore active working set");
  for (const auto &draw : doc.frame()->draws)
    check(draw.mesh == meshes.at(draw.key.actor.id), "retained historical buffers remain reusable");
  options.max_tessellation_cache_entries = 0;
  Document uncached(options); uncached.adopt(f.session()); uncached.set_frame(3);
  const auto unfirst = uncached.frame(); uncached.set_frame(4);
  check(uncached.tessellation_cache_entries() == 0 &&
            unfirst->draws[0].mesh != uncached.frame()->draws[0].mesh,
        "zero tessellation cache keeps its existing disable semantics");
}
void query_geometry_publication_lifetime() {
  for (const auto order : {ByteOrder::little, ByteOrder::big}) {
    scene_query_test::Fixture f(order);
    const auto marker = f.str("frame");
    f.emit(BeginSection{f.scene, marker});
    f.publish();
    const auto publication_end = f.events.size();
    // PVD query geometry is transient. Reuse its address before EndFrame to
    // ensure the preview uses the publication's generation and dimensions.
    f.emit(DestroyInstance{f.box});
    f.emit(CreateInstance{f.type("PxBoxGeometry"), f.box});
    scene_query_test::BytesWriter huge(order);
    huge.v({100, 200, 300});
    f.set(f.box, "HalfExtents", "PxVec3", huge.bytes, 1);
    f.emit(EndSection{f.scene, marker});
    Document doc;
    doc.adopt(std::make_unique<Session>(f.header, f.events));
    check(doc.set_scene(f.scene), "query lifetime scene");
    const auto frame = doc.frame();
    check(frame && frame->snapshot.instance(f.box)->generation == 1 &&
              frame->query_volumes.size() == 2,
          "transient query geometry survives native frame-end reuse");
    for (const auto &volume : frame->query_volumes) {
      check(volume.source.snapshot_event_count <= publication_end && volume.mesh,
            "query preview uses its exact publication prefix");
      Vec3 extents{};
      for (const auto p : volume.mesh->positions) {
        extents.x = std::max(extents.x, std::abs(p.x));
        extents.y = std::max(extents.y, std::abs(p.y));
        extents.z = std::max(extents.z, std::abs(p.z));
      }
      near(extents, {1, 2, 3}, "query preview retains old geometry dimensions");
    }
  }
}
void real_capture(const char *path) {
  Document doc;
  doc.open_file(path);
  check(doc.loaded() && !doc.scenes().empty() && !doc.timeline().empty(),
        "real recording loads with scenes and frames");
  check(doc.frame()->errors.empty() && doc.frame()->render.diagnostics.empty(),
        "real geometry builds without errors");
  std::set<render::GeometryKind> kinds;
  for (const auto &s : doc.frame()->render.instances)
    kinds.insert(render::geometry_kind(s.geometry));
  check(kinds.size() == 7,
        "real recording contains seven supported geometries");
  check(doc.set_sample_point(SamplePoint::after_fetch),
        "real capture explicit post-fetch markers");
  auto first = doc.frame();
  check(!first->draws.empty(), "real draw triangles exist");
  auto selected = first->draws.back().key;
  check(doc.select(selected), "select real geometry");
  OrbitCamera camera;
  check(doc.fit_selected(camera, 1.5), "fit selected real shape");
  bool picked = false;
  for (const auto &draw : first->draws) {
    if (!draw.pickable || draw.mesh->indices.size() < 3)
      continue;
    auto a = render::transform_point(
             draw.world_pose, draw.mesh->positions[draw.mesh->indices[0]]),
         b = render::transform_point(
             draw.world_pose, draw.mesh->positions[draw.mesh->indices[1]]),
         c = render::transform_point(
             draw.world_pose, draw.mesh->positions[draw.mesh->indices[2]]);
    Vec3 p{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
    Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
    Vec3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    double length =
        std::sqrt(double(n.x) * n.x + double(n.y) * n.y + double(n.z) * n.z);
    if (length <= 0)
      continue;
    n = {float(n.x / length), float(n.y / length), float(n.z / length)};
    Ray ray{{p.x + n.x * 2, p.y + n.y * 2, p.z + n.z * 2}, {-n.x, -n.y, -n.z}};
    if (pick(first->draws, ray)) {
      picked = true;
      break;
    }
  }
  check(picked, "CPU picker hits real tessellated geometry");
  auto count = doc.timeline().size();
  for (std::size_t i = count; i-- > 0;) {
    check(doc.set_frame(i), "real reverse seek");
    check(doc.frame()->snapshot.event_count == doc.timeline()[i].event_count &&
              doc.frame()->errors.empty(),
          "real marker state and tessellation");
  }
  check(first->draws.back().mesh->positions.size() > 0,
        "retained real frame buffers survive seeks");
}
} // namespace
int main(int argc, char **argv) {
  try {
    camera_and_picking();
    tight_fit_and_zoom_clipping();
    document_navigation();
    active_mesh_reuse();
    query_geometry_publication_lifetime();
    if (argc > 1)
      real_capture(argv[1]);
    std::cout << "viewer model tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
