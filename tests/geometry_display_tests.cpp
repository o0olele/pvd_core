#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "display_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "pvd/body_overlays.hpp"
#include "ui_scale.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <wrl/client.h>

namespace {
using namespace pvd;
using namespace pvd::viewer;
using Microsoft::WRL::ComPtr;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void expect_near(double a, double b, const char *message) {
  check(std::isfinite(a) && std::abs(a - b) < 2e-4, message);
}
Bytes numbers(std::initializer_list<float> values) {
  Bytes result;
  for (auto value : values) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    for (unsigned i = 0; i < 4; ++i)
      result.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
  }
  return result;
}
struct Fixture {
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  void emit(Payload value) {
    events.push_back({1, events.size(), 0, std::move(value)});
  }
  std::uint32_t text(const std::string &value) {
    auto found = strings.find(value);
    if (found != strings.end())
      return found->second;
    auto id = static_cast<std::uint32_t>(strings.size() + 1);
    strings[value] = id;
    emit(StringHandleEvent{value, id});
    return id;
  }
  WireName type(const char *name) {
    auto ns = text("physx3");
    return {ns, text(name)};
  }
  void property(const char *cls, const char *name, const char *dt,
                bool array = false) {
    auto c = type(cls), t = type(dt);
    auto n = text(name);
    emit(CreateProperty{
        c, n, 0, t, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void value(std::uint64_t id, const char *name, const char *dt, Bytes data) {
    auto n = text(name);
    auto t = type(dt);
    emit(SetPropertyValue{id, n, t, 1, std::move(data)});
  }
  void ref(std::uint64_t id, const char *name, std::uint64_t target) {
    if (std::strcmp(name, "Shapes") == 0)
      emit(PushBackObjectRef{id, text(name), target});
    else {
      Bytes bytes;
      for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>(target >> (8 * i)));
      value(id, name, "VoidPtr", std::move(bytes));
    }
  }
  void pose(std::uint64_t id, const char *name, float x, float y, float z) {
    value(id, name, "PxTransform", numbers({0, 0, 0, 1, x, y, z}));
  }
  Fixture() {
    for (auto name : {"PxScene", "PxRigidStatic", "PxShape", "PxBoxGeometry",
                      "PxPlaneGeometry"})
      emit(CreateClass{type(name)});
    property("PxRigidStatic", "Scene", "ObjectRef");
    property("PxRigidStatic", "Shapes", "ObjectRef", true);
    property("PxRigidStatic", "GlobalPose", "PxTransform");
    property("PxShape", "Geometry", "ObjectRef");
    property("PxShape", "LocalPose", "PxTransform");
    property("PxShape", "Flags", "PvdU8");
    property("PxBoxGeometry", "HalfExtents", "PxVec3");
    emit(CreateInstance{type("PxScene"), 1});
    emit(CreateInstance{type("PxBoxGeometry"), 200});
    value(200, "HalfExtents", "PxVec3", numbers({1, 2, 3}));
    emit(CreateInstance{type("PxPlaneGeometry"), 201});
    for (unsigned i = 0; i < 2; ++i) {
      auto actor = 10 + i, shape = 100 + i;
      emit(CreateInstance{type("PxShape"), shape});
      ref(shape, "Geometry", 200 + i);
      pose(shape, "LocalPose", 2, 0, 0);
      value(shape, "Flags", "PvdU8", {11});
      emit(CreateInstance{type("PxRigidStatic"), actor});
      ref(actor, "Scene", 1);
      ref(actor, "Shapes", shape);
      pose(actor, "GlobalPose", 10, 3, 4);
    }
    auto frame = text("frame");
    emit(BeginSection{1, frame, 1});
    emit(EndSection{1, frame, 2});
  }
  void load(Document &doc) const {
    StreamHeader h;
    h.stream_flags = 1;
    doc.adopt(std::make_unique<Session>(h, events));
  }
};
DrawItem triangle(bool reverse = false) {
  auto mesh = std::make_shared<render::TriangleBuffer>();
  mesh->positions = {{-.8f, -.7f, 0}, {.8f, -.7f, 0}, {0, .8f, 0}};
  mesh->normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  mesh->indices = reverse ? std::vector<std::uint32_t>{0, 2, 1}
                          : std::vector<std::uint32_t>{0, 1, 2};
  DrawItem draw;
  draw.mesh = mesh;
  draw.key = {{1, 0}, {10, 0}, {100, 0}};
  draw.color = {230, 60, 40, 255};
  return draw;
}
void model() {
  Fixture fixture;
  Document doc;
  fixture.load(doc);
  const auto source = doc.frame();
  const auto key = source->draws.front().key;
  const auto raw_pose =
      source->snapshot.instance(10)->property("GlobalPose")->raw_bytes();
  const auto raw_bounds = source->render.instances.front().world_bounds;
  const auto mesh = source->draws.front().mesh;
  const auto source_vertices = mesh->positions;
  auto options = doc.view_options();
  options.show_planes = false;
  options.geometry_scale = 2;
  doc.set_view_options(options);
  const auto frame = doc.frame();
  check(frame->draws[0].mesh == mesh && frame->draws[0].visible &&
            !frame->draws[1].visible,
        "scaling preserves cached mesh identity and plane filtering");
  const auto bounds = display_bounds(frame->draws[0]);
  check(bounds.has_value(), "scaled bounds available");
  expect_near(bounds->minimum.x, 10,
              "scale is about shape origin, not scene origin");
  expect_near(bounds->maximum.x, 14, "scaled shape maximum");
  expect_near(bounds->minimum.y, -1, "local extents scaled in world");
  expect_near(bounds->maximum.z, 10, "local shape scale includes all axes");
  check(doc.select(key), "select attachment before display fit");
  OrbitCamera camera;
  check(doc.fit_selected(camera, 1.5), "scaled selected fit");
  expect_near(camera.target.x, 12, "fit uses unchanged shape-pose anchor");
  expect_near(camera.target.y, 3, "fit uses displayed bounds");
  auto hit = pick(doc.frame()->draws, {{13.5f, 3, 20}, {0, 0, -1}});
  check(hit && hit->key == key,
        "pick hits enlarged displayed geometry outside raw face");
  expect_near(hit->position.z, 10,
              "pick returns actual displayed intersection");
  check(source->render.instances.front().world_bounds->maximum.z ==
                raw_bounds->maximum.z &&
            frame->snapshot.instance(10)->property("GlobalPose")->raw_bytes() ==
                raw_pose &&
            frame->render.instances.front().world_pose.position.x == 12 &&
            mesh->positions[0].x == source_vertices[0].x,
        "display scaling cannot alter source snapshot, physical bounds, pose "
        "or vertices");
  const auto accepted = doc.frame();
  for (float bad :
       {0.f, -1.f, .009f, 10001.f, std::numeric_limits<float>::infinity()}) {
    options.geometry_scale = bad;
    bool threw = false;
    try {
      doc.set_view_options(options);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    check(threw && doc.frame() == accepted &&
              doc.view_options().geometry_scale == 2,
          "invalid display scale changes neither view nor source");
  }
  options = doc.view_options();
  options.geometry_scale = 1;
  options.show_planes = true;
  doc.set_view_options(options);
  expect_near(display_bounds(doc.frame()->draws[0])->maximum.z,
              raw_bounds->maximum.z,
              "default scale retains original fit bounds");
  check(doc.frame()->draws[1].visible,
        "planes can be restored without reloading");
  std::cout << "Geometry display: local scale/fit/pick/planes/cache/source "
               "preservation passed\n";
}
void navigation_failures() {
  Fixture f;
  const auto seed = f.events.size();
  f.emit(CreateInstance{f.type("PxScene"), 2});
  f.emit(CreateInstance{f.type("PxBoxGeometry"), 202});
  f.value(202, "HalfExtents", "PxVec3", numbers({2e35f, 2e35f, 2e35f}));
  f.emit(CreateInstance{f.type("PxShape"), 102});
  f.ref(102, "Geometry", 202);
  f.pose(102, "LocalPose", 0, 0, 0);
  f.value(102, "Flags", "PvdU8", {11});
  f.emit(CreateInstance{f.type("PxRigidStatic"), 12});
  f.ref(12, "Scene", 2);
  f.ref(12, "Shapes", 102);
  f.pose(12, "GlobalPose", 0, 0, 0);
  const auto native = f.text("frame"), after = f.text("validation_after_fetch");
  f.emit(BeginSection{2, native, 3});
  f.emit(EndSection{2, native, 4});
  f.value(200, "HalfExtents", "PxVec3", numbers({2e35f, 2e35f, 2e35f}));
  f.emit(BeginSection{1, native, 5});
  f.emit(EndSection{1, native, 6});
  f.emit(BeginSection{1, after, 7});
  f.emit(EndSection{1, after, 8});
  Document doc;
  f.load(doc);
  auto options = doc.view_options();
  options.geometry_scale = 10000;
  doc.set_view_options(options);
  check(doc.select(doc.frame()->draws[0].key), "overflow navigation selection");
  doc.set_playback({true, false, 60});
  const auto reject = [&](auto action) {
    const auto before = doc.frame();
    const auto index = doc.frame_index();
    const auto scene = doc.scene_id();
    const auto mode = doc.sample_point();
    const auto selected = doc.selected_key();
    const auto inspected = doc.inspected_object();
    const auto count = doc.timeline().size();
    const auto playing = doc.playback().playing;
    bool failed = false;
    try {
      action();
    } catch (const std::overflow_error &) {
      failed = true;
    }
    check(failed && doc.frame() == before && doc.frame_index() == index &&
              doc.scene_id() == scene && doc.sample_point() == mode &&
              doc.timeline().size() == count &&
              doc.selected_key() == selected &&
              doc.inspected_object() == inspected &&
              doc.playback().playing == playing,
          "failed display navigation must atomically retain "
          "cursor/frame/timeline/selection/playback");
  };
  reject([&] { doc.set_frame(1); });
  reject([&] { doc.set_frame(1); });
  reject([&] { doc.step(1); });
  reject([&] { doc.set_scene(2); });
  reject([&] { doc.set_sample_point(SamplePoint::after_fetch); });
  reject([&] { doc.advance(1. / 60); });
  options.geometry_scale = 1;
  doc.set_view_options(options);
  check(doc.advance(1. / 60) && doc.frame_index() == 1 &&
            doc.frame()->frame_index == 1,
        "playback retry remains usable after lowering scale");
  check(doc.set_scene(2) && doc.scene_id() == 2,
        "scene retry succeeds after repairing display setting");
  StreamHeader header;
  header.stream_flags = 1;
  const std::vector<Event> first(f.events.begin(), f.events.begin() + seed),
      tail(f.events.begin() + seed, f.events.end());
  Document live;
  live.begin_live(header);
  options = live.view_options();
  options.geometry_scale = 10000;
  live.set_view_options(options);
  live.append_live(first);
  const auto old = live.frame();
  const auto old_index = live.frame_index();
  live.append_live(tail);
  check(live.live_event_count() == f.events.size() &&
            live.timeline().size() == 2 && live.frame_index() == old_index &&
            live.frame()->frame_index == old_index &&
            live.frame()->snapshot.event_count == old->snapshot.event_count &&
            !live.follow_live() && !live.frame()->errors.empty(),
        "live display overflow pauses preview while accepting source and "
        "retaining consistent historical sample");
  live.set_follow_live(true);
  check(!live.follow_live() && live.frame_index() == old_index,
        "unrepaired live follow retries do not cancel or corrupt accepted "
        "source");
  options.geometry_scale = 1;
  live.set_view_options(options);
  live.set_follow_live(true);
  check(live.follow_live() && live.frame_index() == 1 &&
            live.frame()->frame_index == 1 &&
            live.live_event_count() == f.events.size(),
        "live follow recovers after lowering display scale without replaying "
        "accepted events");
  live.finish_live();
  std::cout << "Geometry display failure recovery: "
               "seek/scene/sample/playback/live prefix and retry passed\n";
}
std::size_t colored(const RgbaImage &image) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < image.pixels.size(); i += 4)
    count += image.pixels[i] > 40 || image.pixels[i + 1] > 40 ||
             image.pixels[i + 2] > 40;
  return count;
}
RenderOptions quiet() {
  RenderOptions options;
  options.background_color = {0, 0, 0, 1};
  options.show_grid = options.show_axes = options.show_debug_points =
      options.show_debug_lines = options.show_debug_triangles =
          options.show_contacts = options.show_joint_frames =
              options.show_joint_limits = options.show_scene_queries =
                  options.show_cloth = options.show_particles = false;
  return options;
}
void room_cutaway(Renderer &renderer, ID3D11Device *device,
                  ID3D11DeviceContext *context) {
  // A collision room has inward-facing walls and ceiling. From outside, its
  // near wall must disappear while the far wall and interior solid stay opaque.
  auto room_mesh = std::make_shared<render::TriangleBuffer>(
      render::tessellate(render::Box{{2, 2, 2}}));
  for (std::size_t i = 0; i < room_mesh->indices.size(); i += 3)
    std::swap(room_mesh->indices[i + 1], room_mesh->indices[i + 2]);
  for (auto &normal : room_mesh->normals) {
    normal.x = -normal.x;
    normal.y = -normal.y;
    normal.z = -normal.z;
  }
  const auto source_indices = room_mesh->indices;
  DrawItem room;
  room.mesh = room_mesh;
  room.key = {{1, 0}, {10, 0}, {100, 0}};
  room.color = {230, 30, 30, 255};
  DrawItem body;
  body.mesh = std::make_shared<render::TriangleBuffer>(
      render::tessellate(render::Box{{.5f, .5f, .5f}}));
  body.key = {{1, 0}, {11, 0}, {101, 0}};
  body.color = {30, 30, 230, 255};
  FrameState frame;
  frame.draws = {room, body};
  OrbitCamera camera;
  camera.set_look_at({{0, 0, 5}, {0, 1, 0}, {0, 0, 0}});
  // Sample away from face diagonals so the wire overlay cannot mask the fill.
  const auto center = (std::size_t(125) * 320 + 174) * 4;
  for (auto hand :
       {DisplayHandedness::right_handed, DisplayHandedness::left_handed}) {
    camera.handedness = hand;
    for (bool wireframe : {false, true}) {
      auto options = quiet();
      check(options.face_culling == FaceCulling::back,
            "new view defaults must expose inward-facing rooms");
      options.show_wireframe = wireframe;
      renderer.render(frame, camera, options);
      auto image = readTextureRgba8(device, context, renderer.texture());
      auto hit = pick(visualization_pick_draws(frame, options),
                      camera.screen_ray(174, 125, 320, 240));
      check(image.pixels[center + 2] > image.pixels[center] && hit &&
                hit->key == body.key,
            "room cutaway reveals and picks opaque interior body in both hands");
      const auto uploaded = renderer.stats().cache_misses;
      options.face_culling = FaceCulling::none;
      renderer.render(frame, camera, options);
      image = readTextureRgba8(device, context, renderer.texture());
      hit = pick(visualization_pick_draws(frame, options),
                 camera.screen_ray(174, 125, 320, 240));
      check(image.pixels[center] > image.pixels[center + 2] && hit &&
                hit->key == room.key &&
                renderer.stats().cache_misses == uploaded,
            "None restores two-sided occlusion and picking without mesh uploads");
    }
  }
  check(room_mesh->indices == source_indices &&
            frame.draws[0].mesh == room_mesh && room.color.a == 255 &&
            body.color.a == 255,
        "cutaway must preserve recorded winding, mesh identity and opacity");
  std::cout << "Room cutaway WARP/CPU: inward shell, opaque body, both hands, "
               "wireframe and two-sided override passed\n";
}
void gpu() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                    nullptr, 0, D3D11_SDK_VERSION, &device,
                                    nullptr, &context)),
        "WARP device");
  Renderer renderer(device.Get(), context.Get());
  renderer.resize(320, 240);
  room_cutaway(renderer, device.Get(), context.Get());
  OrbitCamera camera;
  camera.set_look_at({{0, 0, 5}, {0, 1, 0}, {0, 0, 0}});
  for (auto hand :
       {DisplayHandedness::right_handed, DisplayHandedness::left_handed}) {
    camera.handedness = hand;
    for (bool reversed : {false, true}) {
      FrameState frame;
      frame.draws = {triangle(reversed)};
      for (auto mode :
           {FaceCulling::none, FaceCulling::front, FaceCulling::back}) {
        auto options = quiet();
        options.face_culling = mode;
        renderer.render(frame, camera, options);
        const auto pixels = colored(
            readTextureRgba8(device.Get(), context.Get(), renderer.texture()));
        const auto hit = pick(visualization_pick_draws(frame, options),
                              camera.screen_ray(160, 120, 320, 240));
        check((pixels > 20) == bool(hit),
              "GPU face culling and CPU pick agree for both hands/windings");
        check(bool(hit) == (mode == FaceCulling::none ||
                            (reversed ? mode == FaceCulling::front
                                      : mode == FaceCulling::back)),
              "front is the recorded triangle's winding, independent of "
              "display mirror");
      }
    }
  }
  FrameState frame;
  frame.draws = {triangle()};
  auto options = quiet();
  renderer.render(frame, camera, options);
  const auto first = colored(
      readTextureRgba8(device.Get(), context.Get(), renderer.texture()));
  const auto misses = renderer.stats().cache_misses;
  frame.draws[0].geometry_scale = 2;
  renderer.render(frame, camera, options);
  check(colored(readTextureRgba8(device.Get(), context.Get(),
                                 renderer.texture())) > first * 3 &&
            renderer.stats().cache_misses == misses,
        "GPU scale changes area without uploading a new mesh");
  FrameState empty;
  renderer.render(empty, camera, options);
  check(colored(readTextureRgba8(device.Get(), context.Get(),
                                 renderer.texture())) == 0,
        "quiet empty viewport");
  options.show_orbit_target = true;
  renderer.render(empty, camera, options);
  check(colored(readTextureRgba8(device.Get(), context.Get(),
                                 renderer.texture())) > 5 &&
            renderer.stats().orbit_target_markers == 1,
        "orbit target toggle draws actual GPU marker");
  FrameState translucent;
  auto red = triangle(), blue = triangle();
  auto moved = std::make_shared<render::TriangleBuffer>(*red.mesh);
  for (auto &p : moved->positions)
    p.z = 1;
  red.mesh = moved;
  red.color = {230, 30, 30, 128};
  blue.key.shape.id = 101;
  blue.color = {30, 30, 230, 128};
  blue.world_pose.position.z = 1.5f;
  translucent.draws = {red, blue};
  options = quiet();
  renderer.render(translucent, camera, options);
  auto image =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  const auto center = (std::size_t(120) * 320 + 160) * 4;
  check(image.pixels[center + 2] > image.pixels[center],
        "near blue alpha face composited last");
  translucent.draws[0].geometry_scale = 2;
  renderer.render(translucent, camera, options);
  image = readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  check(image.pixels[center] > image.pixels[center + 2],
        "alpha sorting follows scaled local mesh center rather than actor "
        "anchor");
  Fixture fixture;
  Document doc;
  fixture.load(doc);
  auto view = doc.view_options();
  view.geometry_scale = 2;
  view.show_planes = false;
  doc.set_view_options(view);
  check(doc.fit_scene(camera, 4. / 3), "display-bounds camera fit");
  options = quiet();
  options.show_bounds = true;
  renderer.render(*doc.frame(), camera, options);
  const auto &overlay = renderer.body_overlays();
  check(overlay.complete && overlay.bounds_count == 1,
        "scaled visible attachment bounds overlay");
  float min_y = std::numeric_limits<float>::infinity(), max_z = -min_y;
  for (const auto &line : overlay.bounds)
    for (auto p : {line.position0, line.position1}) {
      min_y = std::min(min_y, p.y);
      max_z = std::max(max_z, p.z);
    }
  expect_near(min_y, -1, "display bounds overlay matches scaled minimum");
  expect_near(max_z, 10, "display bounds overlay matches scaled maximum");
  FrameState particles;
  particles.snapshot.event_count = 12;
  deformables::ParticleCloud front, back;
  front.key = {{1, 0}, {20, 0}};
  back.key = {{1, 0}, {21, 0}};
  front.kind = deformables::ParticleKind::system;
  back.kind = deformables::ParticleKind::fluid;
  deformables::Particle p;
  p.index = 17;
  p.position = Vec3{0, 0, 2};
  front.particles = {p};
  p.index = 35;
  p.position = Vec3{0, 0, 0};
  back.particles = {p};
  particles.deformables.particle_clouds = {front, back};
  deformables::ClothSurface cloth;
  cloth.key = {{1, 0}, {22, 0}};
  cloth.local_positions = cloth.world_positions = {{0, 0, 3}};
  cloth.inverse_masses = {0};
  particles.deformables.cloths = {cloth};
  particles.deformable_draws = {{front.key, true, false},
                                {back.key, true, false},
                                {cloth.key, true, true}};
  particles.deformable_selection = deformables::SelectionKey{
      cloth.key, deformables::ElementKind::cloth_vertex, 0, 12};
  particles.palette.particle[3] = particles.palette.cloth[3] = 0;
  const Ray ray{{0, 0, 5}, {0, 0, -1}};
  auto chosen = pick_deformable(particles, ray);
  check(chosen && chosen->key.object == back.key && chosen->key.index == 35,
        "zero-alpha foreground particle and selected pinned cloth do not "
        "intercept visible fluid");
  camera.set_look_at({{0, 0, 5}, {0, 1, 0}, {0, 0, 0}});
  options = quiet();
  options.show_cloth = options.show_particles = true;
  renderer.render(particles, camera, options);
  check(renderer.stats().particle_points == 1 &&
            renderer.stats().cloth_points == 0 &&
            colored(readTextureRgba8(device.Get(), context.Get(),
                                     renderer.texture())) > 0,
        "GPU excludes alpha-zero owners including selected pins");
  particles.deformables.particle_clouds[0].color = Color{255, 0, 0, 255};
  chosen = pick_deformable(particles, ray);
  check(
      chosen && chosen->key.object == front.key,
      "recorded opaque owner color overrides zero-alpha palette consistently");
  particles.palette.particle[3] = 1;
  particles.deformables.particle_clouds[0].color = Color{255, 0, 0, 0};
  chosen = pick_deformable(particles, ray);
  check(
      chosen && chosen->key.object == back.key,
      "recorded transparent owner color overrides opaque palette for picking");
  particles.deformables.particle_clouds.pop_back();
  particles.draws = {triangle()};
  check(!pick_deformable(particles, ray) && pick(particles.draws, ray),
        "transparent native points allow a visible rigid shape behind to be "
        "picked");
  std::cout << "Geometry display WARP: both winding/hands/culling, GPU "
               "area/cache and target marker passed\n";
  std::cout << "Geometry display WARP: scaled alpha ordering/bounds and owner "
               "alpha-zero picking passed\n";
}
void projection_clipping() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                    nullptr, 0, D3D11_SDK_VERSION, &device,
                                    nullptr, &context)),
        "WARP clip device");
  Renderer renderer(device.Get(), context.Get());
  renderer.resize(320, 240);
  DrawItem box;
  box.mesh = std::make_shared<const render::TriangleBuffer>(
      render::tessellate(render::Box{{1, 1, 1}}));
  box.key = {{1, 0}, {10, 0}, {100, 0}};
  box.color = {220, 90, 60, 255};
  FrameState frame;
  frame.draws = {box};
  const auto vertices = box.mesh->positions;
  OrbitCamera camera;
  camera.set_look_at({{0, 0, 5}, {0, 1, 0}, {0, 0, 0}});
  const auto center = (std::size_t(120) * 320 + 160) * 4;
  for (auto hand :
       {DisplayHandedness::right_handed, DisplayHandedness::left_handed}) {
    camera.handedness = hand;
    for (unsigned clip_case = 0; clip_case < 4; ++clip_case) {
      camera.near_plane = clip_case == 0 ? 8 : clip_case == 2 ? 5 : .1;
      camera.far_plane = clip_case == 1 ? 3 : clip_case == 3 ? 5 : 10;
      for (auto mode :
           {FaceCulling::none, FaceCulling::front, FaceCulling::back}) {
        auto options = quiet();
        options.face_culling = mode;
        renderer.render(frame, camera, options);
        const auto image =
            readTextureRgba8(device.Get(), context.Get(), renderer.texture());
        const bool drawn = image.pixels[center] > 40 ||
                           image.pixels[center + 1] > 40 ||
                           image.pixels[center + 2] > 40;
        const auto ray = camera.screen_ray(160, 120, 320, 240);
        const auto hit = pick(visualization_pick_draws(frame, options), ray);
        const bool expected = clip_case < 2    ? false
                              : clip_case == 2 ? mode != FaceCulling::back
                                               : mode != FaceCulling::front;
        check(drawn == expected && bool(hit) == expected,
              "GPU center sample and rigid camera-ray clipping/culling must "
              "agree");
        if (hit)
          expect_near(hit->distance, clip_case == 2 ? 6 : 4,
                      "near-cut front cannot conceal the next visible back "
                      "intersection");
        if (clip_case < 2)
          check(colored(image) == 0, "fully clipped box has no GPU pixels");
      }
    }
    // Offset native centers are within the marker's angular selection radius.
    // Their actual view depth, not projection onto the ray, defines clipping.
    camera.near_plane = 8;
    camera.far_plane = 10;
    const double x =
        160 * (1 + .5 / (std::tan(camera.vertical_fov / 2) * (4. / 3)));
    const auto ray = camera.screen_ray(x, 120, 320, 240);
    const float sign = ray.direction.x > 0 ? 1.f : -1.f;
    const Vec3 positions[] = {{sign * 4.05f, 0, 5 - 7.99f},
                              {sign * 3.95f, 0, 5 - 8.01f},
                              {sign * 4.95f, 0, 5 - 10.01f},
                              {sign * 5.05f, 0, 5 - 9.99f}};
    for (bool cloth_case : {false, true})
      for (unsigned i = 0; i < 4; ++i) {
        FrameState points;
        const deformables::OwnerKey key{{1, 0}, {20, 0}};
        points.deformable_draws = {{key, true, false}};
        if (cloth_case) {
          deformables::ClothSurface cloth;
          cloth.key = key;
          cloth.world_positions = {positions[i]};
          points.deformables.cloths = {cloth};
        } else {
          deformables::ParticleCloud cloud;
          cloud.key = key;
          deformables::Particle p;
          p.index = 17;
          p.position = positions[i];
          cloud.particles = {p};
          points.deformables.particle_clouds = {cloud};
        }
        auto options = quiet();
        options.show_particles = options.show_cloth = true;
        options.point_size = 12;
        renderer.render(points, camera, options);
        const auto image =
            readTextureRgba8(device.Get(), context.Get(), renderer.texture());
        DeformablePickOptions tolerance;
        tolerance.world_radius = 0;
        tolerance.angular_radius =
            6 * 2 * std::tan(camera.vertical_fov / 2) / 240;
        const auto hit = pick_deformable(points, ray, tolerance);
        check(bool(hit) == (i % 2 == 1) && (colored(image) > 0) == bool(hit),
              "off-ray native marker centers obey camera depth on both "
              "near/far edges");
        if (i == 0) {
          const auto along = (positions[i].x - ray.origin.x) * ray.direction.x +
                             (positions[i].y - ray.origin.y) * ray.direction.y +
                             (positions[i].z - ray.origin.z) * ray.direction.z;
          check(along > 8 / (-ray.direction.z),
                "off-ray fixture defeats t-only near clipping");
          auto manual = ray;
          manual.view_clip.reset();
          check(pick_deformable(points, manual, tolerance).has_value(),
                "manual rays retain legacy unbounded native picking");
        }
      }
  }
  const Ray manual{{0, 0, 5}, {0, 0, -1}};
  const auto raw_hit = pick(frame.draws, manual);
  check(raw_hit.has_value(), "manual rigid ray remains unbounded");
  expect_near(raw_hit->distance, 4,
              "manual rigid ray still selects physical front");
  auto segment = manual;
  segment.min_distance = 5;
  segment.max_distance = 7;
  const auto back = pick(frame.draws, segment);
  check(back.has_value(), "manual bounded ray admits later intersection");
  expect_near(back->distance, 6,
              "manual distance interval skips clipped first face");
  for (unsigned i = 0; i < 4; ++i) {
    auto bad = manual;
    if (i == 0)
      bad.min_distance = -1;
    if (i == 1)
      bad.max_distance = std::numeric_limits<double>::quiet_NaN();
    if (i == 2)
      bad.view_clip = RayViewClip{{0, 0, 0}, 1, 10};
    if (i == 3)
      bad.view_clip = RayViewClip{{0, 0, -1}, 10, 1};
    bool rejected = false;
    try {
      (void)pick(frame.draws, bad);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    check(rejected, "invalid ray clip policy must reject explicitly");
  }
  Fixture source;
  Document doc;
  source.load(doc);
  const auto raw =
      doc.frame()->snapshot.instance(10)->property("GlobalPose")->raw_bytes();
  camera.near_plane = 8;
  camera.far_plane = 10;
  renderer.render(*doc.frame(), camera, quiet());
  (void)pick(doc.frame()->draws, camera.screen_ray(160, 120, 320, 240));
  check(
      doc.frame()->snapshot.instance(10)->property("GlobalPose")->raw_bytes() ==
              raw &&
          box.mesh->positions[0].x == vertices[0].x,
      "projection clipping cannot modify source pose bytes or mesh resources");
  std::cout << "Camera clipping WARP/CPU: near/far, sliced faces/culling, both "
               "hands, off-ray native centers and manual rays passed\n";
}
struct UiHarness {
  Document doc;
  OrbitCamera camera;
  RenderOptions options;
  DisplayPanelState panel;
  ImGuiWindow *window = nullptr;
  float width = 1060, scale = 1, overflow = 0;
  void frame(std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
    auto &io = ImGui::GetIO();
    io.DisplaySize = {width * scale, 1200 * scale};
    io.DeltaTime = 1.f / 60;
    if (mouse)
      io.AddMousePosEvent(mouse->x, mouse->y);
    if (down)
      io.AddMouseButtonEvent(0, *down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({width * scale, 1000 * scale});
    ImGui::Begin("Geometry display controls", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    UiPixelScope pixels(scale);
    drawDisplaySettings(doc, camera, options, {}, panel);
    window = ImGui::GetCurrentWindow();
    overflow =
        std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
    ImGui::End();
    ImGui::Render();
    check(panel.error.empty(), panel.error.c_str());
  }
  void key(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    frame();
    ImGui::GetIO().AddKeyEvent(key, false);
    frame();
  }
  void focus(const char *label, ImGuiWindow *target = nullptr) {
    if (!target)
      target = window;
    const auto id = target->GetID(label);
    for (unsigned i = 0; i < 90; ++i) {
      const auto &g = *ImGui::GetCurrentContext();
      if (g.NavWindow == target && g.NavId == id) {
        frame();
        frame();
        return;
      }
      key(ImGuiKey_Tab);
    }
    throw std::runtime_error(
        std::string("Could not focus actual display widget ") + label);
  }
  ImVec2 focused() const {
    const auto &g = *ImGui::GetCurrentContext();
    const auto &rect = g.NavWindow->NavRectRel[ImGuiNavLayer_Main];
    const ImVec2 p{rect.Min.x + std::min(40 * scale, rect.GetWidth() * .5f),
                   rect.GetCenter().y};
    return {p.x + g.NavWindow->Pos.x, p.y + g.NavWindow->Pos.y};
  }
  void click(ImVec2 point) {
    frame(point, false);
    frame(point, true);
    frame(point, false);
    frame(point, false);
  }
  void drag(const char *label) {
    focus(label);
    const auto point = focused();
    // Tab focuses DragFloat through its real temporary text editor. Leave
    // that editor before exercising a mouse drag, as a user pressing Escape.
    if (ImGui::GetCurrentContext()->TempInputId == window->GetID(label))
      key(ImGuiKey_Escape);
    frame(point, false);
    frame(point, true);
    frame(ImVec2{point.x + 35 * scale, point.y}, true);
    frame(ImVec2{point.x + 70 * scale, point.y}, true);
    frame(ImVec2{point.x + 70 * scale, point.y}, true);
    frame(ImVec2{point.x + 70 * scale, point.y}, false);
    frame();
  }
};
void widgets(float width, float scale) {
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImFontConfig font;
  font.SizePixels = 13 * scale;
  io.Fonts->AddFontDefault(&font);
  unsigned char *pixels;
  int w, h;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  ImGui::GetStyle().ScaleAllSizes(scale);
  UiHarness ui;
  ui.width = width;
  ui.scale = scale;
  Fixture fixture;
  fixture.load(ui.doc);
  const auto raw = ui.doc.frame()
                       ->snapshot.instance(10)
                       ->property("GlobalPose")
                       ->raw_bytes();
  ui.frame();
  ui.frame();
  ui.focus("Show planes");
  ui.click(ui.focused());
  check(!ui.doc.view_options().show_planes && !ui.doc.frame()->draws[1].visible,
        "actual plane checkbox changes visible native attachments");
  ui.drag("Geometry display scale");
  check(ui.doc.view_options().geometry_scale > 1 &&
            ui.doc.frame()->draws[0].geometry_scale > 1,
        "actual geometry scale drag changes model display transform");
  ui.focus("Face culling");
  ui.options.face_culling = FaceCulling::none;
  ui.frame();
  ui.click(ui.focused());
  const auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
  check(popups.Size && popups.back().Window, "actual culling combo opens");
  const auto *menu = popups.back().Window;
  const ImVec2 back{menu->DC.CursorStartPos.x + 10 * scale,
                    menu->DC.CursorStartPos.y +
                        ImGui::GetTextLineHeight() * .5f +
                        2 * ImGui::GetTextLineHeightWithSpacing()};
  ui.click(back);
  check(ui.options.face_culling == FaceCulling::back,
        "actual culling choice applies");
  ui.focus("Orbit target marker");
  ui.click(ui.focused());
  check(ui.options.show_orbit_target,
        "actual orbit target marker checkbox applies");
  ui.drag("Joint frame scale");
  ui.drag("Joint limit scale");
  check(ui.doc.view_options().joint_frame_scale > 1 &&
            ui.doc.view_options().joint_limit_scale > 1,
        "actual independent joint scales change document preference");
  check(ui.doc.frame()
                ->snapshot.instance(10)
                ->property("GlobalPose")
                ->raw_bytes() == raw,
        "mouse controls cannot rewrite captured pose bytes");
  check(ui.overflow <= scale + 1,
        "display controls fit available logical width");
  ImGui::DestroyContext();
  std::cout << "Geometry display actual ImGui width=" << width
            << " scale=" << scale << " passed\n";
}
} // namespace
int main() {
  try {
    model();
    projection_clipping();
    navigation_failures();
    gpu();
    for (auto width : {420.f, 1060.f})
      for (auto scale : {1.f, 1.5f, 2.f})
        widgets(width, scale);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
