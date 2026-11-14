#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "display_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "scene_tools.hpp"
#include "pvd/body_overlays.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <iostream>
#include <map>
#include <stdexcept>
#include <wrl/client.h>

namespace {
using namespace pvd;
using namespace pvd::viewer;
using Microsoft::WRL::ComPtr;
void check(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
void expect_near(double a, double b, const char *m) {
  check(std::isfinite(a) && std::abs(a - b) < 1e-4, m);
}
void expect_near(Vec3 a, Vec3 b, const char *m) {
  expect_near(a.x, b.x, m);
  expect_near(a.y, b.y, m);
  expect_near(a.z, b.z, m);
}
Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale(Vec3 a, float v) { return {a.x * v, a.y * v, a.z * v}; }
std::array<double, 4> project(const Mat4 &m, Vec3 p) {
  std::array<double, 4> v{};
  for (int i = 0; i < 4; ++i)
    v[i] = double(m.values[i]) * p.x + double(m.values[4 + i]) * p.y +
           double(m.values[8 + i]) * p.z + m.values[12 + i];
  return v;
}
Vec3 screen(const OrbitCamera &c, Vec3 p, double width = 800,
            double height = 600) {
  const auto v = project(c.view_projection(width / height), p);
  check(v[3] > 0, "projected point in front of camera");
  return {float((v[0] / v[3] + 1) * width * .5),
          float((1 - v[1] / v[3]) * height * .5), float(v[2] / v[3])};
}
DrawItem triangle(Vec3 center, Vec3 right, Vec3 up) {
  auto mesh = std::make_shared<render::TriangleBuffer>();
  mesh->positions = {add(center, add(scale(right, -.7f), scale(up, -.5f))),
                     add(center, add(scale(right, .8f), scale(up, -.4f))),
                     add(center, scale(up, .9f))};
  mesh->normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  mesh->indices = {0, 1, 2};
  return {mesh,  {},  {{1, 0}, {10, 0}, {100, 0}}, {220, 80, 40, 255}, true,
          false, true};
}
void mathematics() {
  const render::Bounds box{{-4, -3, -2}, {7, 5, 9}};
  for (int up = 0; up < 6; ++up)
    for (auto hand :
         {DisplayHandedness::right_handed, DisplayHandedness::left_handed}) {
      OrbitCamera c;
      c.handedness = hand;
      set_display_up(c, static_cast<DisplayUp>(up));
      check(display_up(c) == static_cast<DisplayUp>(up),
            "six signed up settings roundtrip");
      c.fit(box, 4. / 3);
      for (float x : {box.minimum.x, box.maximum.x})
        for (float y : {box.minimum.y, box.maximum.y})
          for (float z : {box.minimum.z, box.maximum.z}) {
            auto p = project(c.view_projection(4. / 3), {x, y, z});
            check(p[3] > 0 && std::abs(p[0] / p[3]) <= 1 &&
                      std::abs(p[1] / p[3]) <= 1 && p[2] / p[3] >= 0 &&
                      p[2] / p[3] <= 1,
                  "non-Y up and handedness fit encloses all world corners");
          }
      const auto view = c.view_matrix();
      const Vec3 right{view.values[0], view.values[4], view.values[8]},
          vertical{view.values[1], view.values[5], view.values[9]};
      auto item = triangle(c.target, right, vertical);
      const auto center = screen(c, c.target);
      auto ray = c.screen_ray(center.x, center.y, 800, 600);
      auto hit = pick({item}, ray);
      check(hit && hit->key == item.key,
            "screen projection and CPU triangle selection agree under every "
            "display convention");
      expect_near(hit->position, c.target,
                  "picked original source world coordinate");
      const auto source = item.mesh->positions;
      const auto position = c.position();
      auto opposite = c;
      opposite.handedness = hand == DisplayHandedness::right_handed
                                ? DisplayHandedness::left_handed
                                : DisplayHandedness::right_handed;
      expect_near(opposite.position(), position,
                  "display mirror cannot move physical camera");
      const auto point = item.mesh->positions[0];
      auto a = screen(c, point), b = screen(opposite, point);
      expect_near(a.x + b.x, 800,
                  "handedness mirrors horizontal screen coordinate");
      expect_near(a.y, b.y, "handedness retains screen vertical coordinate");
      expect_near(a.z, b.z, "handedness retains depth");
      auto mirrored_hit =
          pick({item}, opposite.screen_ray(800 - center.x, center.y, 800, 600));
      check(mirrored_hit.has_value(),
            "mirrored ray selects same actual source triangle");
      expect_near(mirrored_hit->position, c.target,
                  "mirror ray source position unchanged");
      check(item.mesh->positions[0].x == source[0].x,
            "display never mutates source mesh");
      const auto world = c.target;
      const auto before = screen(c, world);
      c.pan(25, 13, 600);
      const auto after = screen(c, world);
      expect_near(after.x - before.x, 25,
                  "pan follows horizontal mouse motion in both hands");
      expect_near(after.y - before.y, 13,
                  "pan follows vertical mouse motion for all up axes");
      c.orbit(.2, -.1);
      c.zoom(2);
      check(std::isfinite(c.view_projection(4. / 3).values[0]),
            "orbit/zoom remain valid for all signed up axes and hands");
    }
  for (auto hand :
       {DisplayHandedness::right_handed, DisplayHandedness::left_handed}) {
    OrbitCamera c;
    c.handedness = hand;
    const CameraState recorded{{4, 5, 9}, {1, 1, 1}, {1, 2, 3}};
    c.set_look_at(recorded);
    expect_near(c.position(), recorded.position,
                "recorded camera eye retained in both display hands");
    const auto at = screen(c, recorded.target);
    expect_near(at.x, 400, "recorded look-at horizontal center");
    expect_near(at.y, 300, "recorded look-at vertical center");
    const auto ray = c.screen_ray(400, 300, 800, 600);
    const auto delta = subtract(recorded.target, recorded.position);
    const auto length =
        std::sqrt(double(delta.x) * delta.x + double(delta.y) * delta.y +
                  double(delta.z) * delta.z);
    expect_near(ray.direction, scale(delta, float(1 / length)),
                "recorded camera ray follows source target");
    check(!display_up(c), "arbitrary recorded roll is identified as custom");
  }
  auto fallback = select_multisampling(Antialiasing::msaa4x, false, true, true);
  check(fallback.requested_samples == 4 && fallback.actual_samples == 1 &&
            fallback.fallback,
        "unsupported color 4x explicitly falls back");
  check(select_multisampling(Antialiasing::msaa4x, true, false, true).fallback,
        "unsupported depth 4x explicitly falls back");
  check(select_multisampling(Antialiasing::msaa4x, true, true, false).fallback,
        "unsupported resolve explicitly falls back");
  check(select_multisampling(Antialiasing::msaa4x, true, true, true)
                .actual_samples == 4,
        "compatible 4x enabled");
  check(!select_multisampling(Antialiasing::off, false, false, false).fallback,
        "off is intentional 1x, not fallback");
  std::cout
      << "Display axes/mirror/rays/fit/pan/recorded-camera tests passed\n";
}
Bytes ints(std::initializer_list<std::uint64_t> list, std::size_t width = 4) {
  Bytes b;
  for (auto n : list)
    for (std::size_t i = 0; i < width; ++i)
      b.push_back(static_cast<std::uint8_t>(n >> (i * 8)));
  return b;
}
Bytes floats(std::initializer_list<float> list) {
  Bytes b;
  for (auto f : list) {
    std::uint32_t n;
    std::memcpy(&n, &f, 4);
    auto field = ints({n});
    b.insert(b.end(), field.begin(), field.end());
  }
  return b;
}
struct Fixture {
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  void emit(Payload p) {
    events.push_back({1, events.size(), events.size() * 9, std::move(p)});
  }
  std::uint32_t str(const std::string &s) {
    auto found = strings.find(s);
    if (found != strings.end())
      return found->second;
    const auto h = static_cast<std::uint32_t>(strings.size() + 1);
    strings[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *s) {
    auto ns = str("physx3"), n = str(s);
    return {ns, n};
  }
  void prop(const char *c, const char *p, const char *t, bool array = false) {
    auto cls = type(c), dt = type(t);
    auto h = str(p);
    emit(CreateProperty{
        cls, h, 0, dt, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void value(std::uint64_t id, const char *p, const char *t, Bytes b) {
    auto h = str(p);
    auto dt = type(t);
    emit(SetPropertyValue{id, h, dt, 1, std::move(b)});
  }
  explicit Fixture(bool dynamic = false, bool angular = true) {
    const char *actor = dynamic ? "PxRigidDynamic" : "PxRigidStatic";
    for (const char *c : {"PxScene", actor, "PxShape", "PxBoxGeometry"})
      emit(CreateClass{type(c)});
    prop(actor, "Scene", "ObjectRef");
    prop(actor, "Shapes", "ObjectRef", true);
    prop(actor, "GlobalPose", "PxTransform");
    if (dynamic) {
      prop(actor, "RigidBodyFlags", "PvdU32");
      prop(actor, "IsSleeping", "PvdU8");
      prop(actor, "LinearVelocity", "PxVec3");
      if (angular)
        prop(actor, "AngularVelocity", "PxVec3");
    }
    prop("PxShape", "Geometry", "ObjectRef");
    prop("PxShape", "LocalPose", "PxTransform");
    prop("PxShape", "Flags", "PvdU8");
    prop("PxBoxGeometry", "HalfExtents", "PxVec3");
    emit(CreateInstance{type("PxScene"), 1});
    emit(CreateInstance{type(actor), 10});
    emit(CreateInstance{type("PxShape"), 100});
    emit(CreateInstance{type("PxBoxGeometry"), 200});
    if (dynamic) {
      value(10, "RigidBodyFlags", "PvdU32", ints({0}, 4));
      value(10, "IsSleeping", "PvdU8", {0});
      value(10, "LinearVelocity", "PxVec3", floats({5, 0, 0}));
      if (angular)
        value(10, "AngularVelocity", "PxVec3", floats({0, 5, 0}));
    }
    value(10, "Scene", "VoidPtr", ints({1}, 8));
    value(10, "Shapes", "VoidPtr", ints({100}, 8));
    value(100, "Geometry", "VoidPtr", ints({200}, 8));
    value(100, "LocalPose", "PxTransform", floats({0, 0, 0, 1, 0, 0, 0}));
    value(100, "Flags", "PvdU8", {11});
    value(200, "HalfExtents", "PxVec3", floats({1, 2, 3}));
    for (unsigned step = 0; step < 2; ++step) {
      const auto frame = str("frame");
      emit(BeginSection{1, frame, events.size()});
      value(10, "GlobalPose", "PxTransform",
            floats({0, 0, 0, 1, float(step) * 2, float(step) * 3,
                    float(step) * 4}));
      emit(EndSection{1, frame, events.size()});
    }
  }
  void load(Document &doc) {
    StreamHeader h;
    h.stream_flags = 1;
    doc.adopt(std::make_unique<Session>(h, events), "display tracking");
  }
};
void tracking() {
  Fixture fixture;
  Document doc;
  fixture.load(doc);
  OrbitCamera camera;
  set_display_up(camera, DisplayUp::negative_z);
  camera.handedness = DisplayHandedness::left_handed;
  doc.inspect(10);
  doc.fit_selected(camera, 4. / 3);
  const auto eye = camera.position(), target = camera.target;
  const auto source_pose = doc.frame()->draws[0].world_pose.position;
  SceneToolState state;
  check(trackSelected(doc, camera, state),
        "tracking starts with non-Y/LH display");
  doc.set_frame(1);
  check(updateTrackedCamera(doc, camera, state),
        "tracking advances with displayed source lifetime");
  expect_near(camera.target, add(target, {2, 3, 4}),
              "tracking preserves source translation");
  expect_near(camera.position(), add(eye, {2, 3, 4}),
              "tracking moves eye coherently in mirrored display");
  check(camera.handedness == DisplayHandedness::left_handed &&
            display_up(camera) == DisplayUp::negative_z,
        "tracking retains display convention");
  expect_near(source_pose, {0, 0, 0},
              "historical source pose is unchanged by display state");
  doc.set_frame(0);
  check(updateTrackedCamera(doc, camera, state),
        "reverse tracking works under display convention");
  expect_near(camera.position(), eye, "reverse tracking restores physical eye");
}
void constraints(const char *path) {
  ModelOptions options;
  options.debug.joint_axis_length = 9;
  Document doc(options);
  doc.open_file(path);
  const auto frame = doc.frame();
  check(!frame->debug.constraints.empty(),
        "real SDK joint typed source retained");
  const auto expected =
      render::build_constraint_lines(frame->debug.constraints);
  check(expected.complete &&
            frame->joint_frame_lines.size() + frame->joint_limit_lines.size() ==
                expected.lines.size(),
        "model takes over all exact SDK constraint primitives");
  check(!frame->joint_frame_lines.empty() && !frame->joint_limit_lines.empty(),
        "joint frame and limit families separated");
  check(frame->debug.complete == frame->debug.diagnostics.empty(),
        "joint display completeness reflects remaining source diagnostics");
  for (const auto &line : frame->debug.lines)
    check(!line.derived, "legacy joint axes are not generated twice");
  for (const auto &diagnostic : frame->debug.diagnostics)
    check(diagnostic.code != render::DebugCode::constraint_not_tessellated,
          "successful builder replaces its old not-tessellated diagnostic");
  for (const auto *lines :
       {&frame->joint_frame_lines, &frame->joint_limit_lines})
    for (const auto &line : *lines)
      check(line.derived && line.source.channel_instance_id,
            "joint display keeps exact source provenance");
  options.constraints.max_lines = 8;
  Document bounded(options);
  bounded.open_file(path);
  const auto limited = bounded.frame();
  check(limited->joint_frame_lines.size() + limited->joint_limit_lines.size() <=
                8 &&
            !limited->errors.empty(),
        "joint families share one total line budget with visible error");
  check(!limited->debug.complete,
        "limited constraint display is not labeled complete");
  check(limited->debug.constraints.size() == frame->debug.constraints.size(),
        "budget cannot drop typed source constraints");
  check(std::any_of(limited->debug.diagnostics.begin(),
                    limited->debug.diagnostics.end(),
                    [](const auto &diagnostic) {
                      return diagnostic.code ==
                             render::DebugCode::constraint_not_tessellated;
                    }),
        "unhandled limited constraints retain the original availability "
        "diagnostic");
  std::cout << "Real joint frame/limit model integration passed: "
            << expected.lines.size() << " exact lines\n";
}
using Pixel = std::array<std::uint8_t, 4>;
Pixel pixel(const RgbaImage &image, std::uint32_t x, std::uint32_t y) {
  const auto at = (std::size_t(y) * image.width + x) * 4;
  return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2],
          image.pixels[at + 3]};
}
void gpu() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level;
  check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                    nullptr, 0, D3D11_SDK_VERSION, &device,
                                    &level, &context)),
        "real WARP D3D11 device");
  Renderer renderer(device.Get(), context.Get());
  renderer.resize(640, 480);
  OrbitCamera camera;
  camera.yaw = camera.pitch = 0;
  camera.distance = 5;
  RenderOptions options;
  options.show_grid = options.show_axes = options.show_debug_points =
      options.show_debug_lines = options.show_debug_triangles =
          options.show_contacts = false;
  options.background_color = {.2f, .3f, .4f, 1};
  FrameState empty;
  renderer.render(empty, camera, options);
  auto image =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  const auto background = pixel(image, 0, 0);
  check(std::abs(int(background[0]) - 51) <= 1 &&
            std::abs(int(background[1]) - 77) <= 1 &&
            std::abs(int(background[2]) - 102) <= 1 && background[3] == 255,
        "background is actual GPU clear color");
  for (std::uint32_t y = 0; y < image.height; ++y)
    for (std::uint32_t x = 0; x < image.width; ++x)
      check(pixel(image, x, y) == background,
            "empty viewport is uniformly background");
  check(renderer.stats().multisampling.actual_samples == 1 &&
            !renderer.stats().multisampling.fallback,
        "real 1x target");
  FrameState frame;
  frame.draws.push_back(triangle({.3f, .1f, 0}, {1, 0, 0}, {0, 1, 0}));
  renderer.render(frame, camera, options);
  const auto single =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  options.antialiasing = Antialiasing::msaa4x;
  renderer.render(frame, camera, options);
  const auto samples = renderer.stats().multisampling;
  UINT color = 0, depth = 0;
  device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &color);
  device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, 4,
                                        &depth);
  check(color && depth && samples.requested_samples == 4 &&
            samples.actual_samples == 4 && !samples.fallback,
        "WARP uses real 4x color/depth target");
  D3D11_TEXTURE2D_DESC desc{};
  renderer.texture()->GetDesc(&desc);
  check(desc.SampleDesc.Count == 1 && renderer.shader_resource_view(),
        "MSAA resolves to ImGui-compatible single-sample SRV");
  const auto multisampled =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  check(multisampled.pixels != single.pixels,
        "actual 4x coverage differs from 1x at triangle edges");
  auto intermediate = [&](const RgbaImage &picture) {
    std::map<Pixel, std::size_t> counts;
    for (std::uint32_t y = 0; y < picture.height; ++y)
      for (std::uint32_t x = 0; x < picture.width; ++x)
        ++counts[pixel(picture, x, y)];
    counts.erase(background);
    check(!counts.empty(), "real GPU triangle is visible");
    auto fill = std::max_element(
        counts.begin(), counts.end(),
        [](const auto &a, const auto &b) { return a.second < b.second; });
    std::size_t edges = 0;
    for (const auto &p : counts)
      if (p.first != fill->first)
        edges += p.second;
    return edges;
  };
  check(intermediate(single) == 0 && intermediate(multisampled) > 0,
        "MSAA produces real intermediate edge coverage");
  check(renderer.stats().cache_hits > 0 && renderer.stats().cached_meshes == 1,
        "display settings reuse existing GPU geometry");
  options.antialiasing = Antialiasing::off;
  renderer.render(frame, camera, options);
  const auto right =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  camera.handedness = DisplayHandedness::left_handed;
  renderer.render(frame, camera, options);
  const auto left =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  std::size_t different = 0;
  for (std::uint32_t y = 0; y < right.height; ++y)
    for (std::uint32_t x = 0; x < right.width; ++x)
      different += pixel(left, x, y) != pixel(right, right.width - 1 - x, y);
  check(left.pixels != right.pixels &&
            different < right.width * right.height / 1000,
        "actual GPU mirror preserves winding and face lighting");
  frame.draws[0].selected = true;
  renderer.render(frame, camera, options);
  const auto selected =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  check(selected.pixels != left.pixels,
        "selection highlight visible in mirrored viewport");
  const auto &positions = frame.draws[0].mesh->positions;
  const auto target =
      scale(add(add(positions[0], positions[1]), positions[2]), 1.f / 3.f);
  const auto at = screen(camera, target, 640, 480);
  check(pick(frame.draws, camera.screen_ray(at.x, at.y, 640, 480)).has_value(),
        "mirrored GPU position is CPU-pickable");
  options.show_grid = true;
  options.show_axes = true;
  set_display_up(camera, DisplayUp::positive_z);
  camera.yaw = .7;
  camera.pitch = .4;
  renderer.render(empty, camera, options);
  const auto zup =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  check(renderer.stats().line_segments == 85,
        "signed-up grid and three physical axes render");
  set_display_up(camera, DisplayUp::negative_x);
  renderer.render(empty, camera, options);
  const auto xup =
      readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  check(xup.pixels != zup.pixels, "non-Y up changes actual grid/axis display");
  // Exercise the public screenshot helper on an actual multisampled texture,
  // independently of the renderer's already-resolved SRV path.
  desc = {};
  desc.Width = 33;
  desc.Height = 17;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 4;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11RenderTargetView> rtv;
  check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
            SUCCEEDED(
                device->CreateRenderTargetView(texture.Get(), nullptr, &rtv)),
        "actual standalone 4x source");
  context->ClearRenderTargetView(rtv.Get(), options.background_color.data());
  auto readback = readTextureRgba8(device.Get(), context.Get(), texture.Get());
  check(readback.width == 33 && readback.height == 17 &&
            pixel(readback, 32, 16) == background,
        "real multisample readback resolves correctly with odd row dimensions");
  renderer.resize(0, 0);
  check(!renderer.texture() && !renderer.shader_resource_view(),
        "zero-size releases both sample paths");
  renderer.resize(211, 137);
  options.antialiasing = Antialiasing::msaa4x;
  renderer.render(empty, camera, options);
  check(renderer.stats().multisampling.actual_samples == 4 &&
            renderer.width() == 211,
        "resizing reconstructs real 4x viewport");
  options.antialiasing = Antialiasing::off;
  options.show_grid = options.show_axes = false;
  camera = {};
  camera.yaw = camera.pitch = 0;
  camera.distance = 5;
  FrameState joints;
  joints.joint_frame_lines.push_back(
      {{}, {-1, 0, 0}, {-1, 1, 0}, 0xffff0000, 0xffff0000, true});
  joints.joint_limit_lines.push_back(
      {{}, {1, 0, 0}, {1, 1, 0}, 0xff00ff00, 0xff00ff00, true});
  renderer.render(joints, camera, options);
  check(renderer.stats().line_segments == 2,
        "joint families render independently of debug-line switch");
  options.show_joint_frames = false;
  renderer.render(joints, camera, options);
  check(renderer.stats().line_segments == 1, "joint frame visibility mask");
  options.show_joint_limits = false;
  renderer.render(joints, camera, options);
  check(renderer.stats().line_segments == 0, "joint limit visibility mask");
  Fixture body_fixture(true);
  Document bodies;
  body_fixture.load(bodies);
  check(bodies.fit_scene(camera, 211. / 137), "body overlay fit");
  options = {};
  options.show_grid = options.show_axes = options.show_solids = false;
  renderer.render(*bodies.frame(), camera, options);
  const auto no_overlays = readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  check(renderer.body_overlays().processed_attachments == 0,
        "disabled body layers never traverse source properties");
  options.show_bounds = options.show_linear_velocity = options.show_angular_velocity = true;
  renderer.render(*bodies.frame(), camera, options);
  const auto body_image = readTextureRgba8(device.Get(), context.Get(), renderer.texture());
  check(renderer.stats().body_bounds == 1 &&
            renderer.stats().linear_velocity_vectors == 1 &&
            renderer.stats().angular_velocity_vectors == 1 &&
            renderer.stats().line_segments == 18 && renderer.body_overlays().complete,
        "real renderer integrates all three body overlay layers");
  check(body_image.pixels != no_overlays.pixels,
        "actual GPU body boxes and arrows change the image");
  const auto body_key = bodies.frame()->draws.front().key;
  bodies.set_visible(body_key, false);
  renderer.render(*bodies.frame(), camera, options);
  check(renderer.stats().line_segments == 0 && renderer.body_overlays().complete &&
            renderer.body_overlays().diagnostics.empty(),
        "hidden actor cannot leak a bound, velocity or diagnostic");
  check(readTextureRgba8(device.Get(), context.Get(), renderer.texture()).pixels == no_overlays.pixels,
        "hiding last attachment restores original GPU image");
  Fixture missing_angular(true, false);
  Document incomplete;
  missing_angular.load(incomplete);
  renderer.render(*incomplete.frame(), camera, options);
  check(!renderer.body_overlays().complete &&
            renderer.stats().linear_velocity_vectors == 1 &&
            renderer.stats().angular_velocity_vectors == 0 &&
            !renderer.body_overlays().diagnostics.empty(),
        "missing requested source field is exposed with valid independent layers");
  std::cout
      << "Actual WARP 1x/4x/background/resolve/mirror/lighting/selection/grid "
         "tests passed\n";
}
struct Output {
  std::string text;
  float overflow = 0;
};
Output draw(Document &doc, OrbitCamera &camera, RenderOptions &options,
            RenderStats stats, DisplayPanelState &panel, float width,
            std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
  auto &io = ImGui::GetIO();
  io.DisplaySize = {width, 700};
  io.DeltaTime = 1.f / 60;
  if (mouse)
    io.AddMousePosEvent(mouse->x, mouse->y);
  if (down)
    io.AddMouseButtonEvent(0, *down);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({width, 650});
  ImGui::Begin("Display settings test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::LogToBuffer();
  drawDisplaySettings(doc, camera, options, stats, panel);
  Output output;
  output.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  const auto *window = ImGui::GetCurrentWindow();
  output.overflow =
      std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
  ImGui::End();
  ImGui::Render();
  return output;
}
void widgets(float width) {
  Fixture fixture;
  Document doc;
  fixture.load(doc);
  OrbitCamera camera;
  RenderOptions options;
  RenderStats stats;
  DisplayPanelState panel;
  draw(doc, camera, options, stats, panel, width);
  auto output = draw(doc, camera, options, stats, panel, width);
  check(output.text.find("actual 1x") != std::string::npos &&
            output.text.find("exports stay unchanged") != std::string::npos &&
            output.text.find("Tight geometry bounds") != std::string::npos &&
            output.text.find("Angular velocity") != std::string::npos,
        "real ImGui displays actual mode and source/display contract");
  options.antialiasing = Antialiasing::msaa4x;
  stats.multisampling =
      select_multisampling(Antialiasing::msaa4x, false, true, true);
  output = draw(doc, camera, options, stats, panel, width);
  check(output.text.find("Device lacks compatible 4x") != std::string::npos &&
            output.text.find("actual 1x") != std::string::npos,
        "real UI shows explicit unsupported-device fallback");
  stats.multisampling = {};
  output = draw(doc, camera, options, stats, panel, width);
  check(output.text.find("awaiting viewport render") != std::string::npos,
        "pending request is not mislabeled actual 4x");
  // Click the real AA combo to request 1x. This exercises widgets rather than
  // just assigning the renderer option from the test.
  const auto &style = ImGui::GetStyle();
  const ImVec2 combo{style.WindowPadding.x + 40,
                     style.WindowPadding.y +
                         ImGui::GetFrameHeightWithSpacing() +
                         ImGui::GetFrameHeight() * .5f};
  draw(doc, camera, options, stats, panel, width, combo, false);
  draw(doc, camera, options, stats, panel, width, combo, true);
  draw(doc, camera, options, stats, panel, width, combo, false);
  draw(doc, camera, options, stats, panel, width, combo, false);
  const auto *popup = ImGui::GetCurrentContext()->OpenPopupStack.Size
                          ? ImGui::FindWindowByName("##Combo_00")
                          : nullptr;
  check(popup, "real AA combo opens");
  const ImVec2 first{popup->DC.CursorStartPos.x + 20,
                     popup->DC.CursorStartPos.y +
                         ImGui::GetTextLineHeight() * .5f};
  draw(doc, camera, options, stats, panel, width, first, false);
  draw(doc, camera, options, stats, panel, width, first, true);
  draw(doc, camera, options, stats, panel, width, first, false);
  check(options.antialiasing == Antialiasing::off,
        "actual AA combo updates renderer request");
  check(output.overflow <= 1, "display controls fit available width");
  std::cout << "Real display ImGui width=" << width << " passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    mathematics();
    tracking();
    if (argc == 2)
      constraints(argv[1]);
    else
      check(argc == 1,
            "usage: pvd_display_settings_tests [joints_capture.pxd2]");
    gpu();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    widgets(1060);
    widgets(420);
    ImGui::DestroyContext();
    std::cout << "Display settings tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
