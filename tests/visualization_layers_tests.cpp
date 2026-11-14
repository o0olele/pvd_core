#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "display_panel.hpp"
#include "imgui_internal.h"
#include "pvd/body_overlays.hpp"
#include "pvd/coordinate_frames.hpp"
#include "ui_scale.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <iostream>
#include <stdexcept>
#include <wrl/client.h>

namespace {
using namespace pvd;
using namespace pvd::viewer;
namespace r = pvd::render;
using Microsoft::WRL::ComPtr;
void check(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
PropertyValue floats(const char *type, std::initializer_list<float> values) {
  PropertyValue p;
  p.incoming_type = {"physx3", type};
  p.item_count = 1;
  Bytes bytes(values.size() * 4);
  std::size_t offset = 0;
  for (float value : values) {
    std::memcpy(bytes.data() + offset, &value, 4);
    offset += 4;
  }
  p.bytes = std::make_shared<const Bytes>(std::move(bytes));
  return p;
}
PropertyValue references(std::initializer_list<std::uint64_t> ids) {
  PropertyValue p;
  p.incoming_type = {"physx3", "VoidPtr"};
  p.item_count = static_cast<std::uint32_t>(ids.size());
  p.reference_ids = std::vector<std::uint64_t>(ids);
  Bytes bytes;
  for (auto id : ids)
    for (unsigned i = 0; i < 8; ++i)
      bytes.push_back(static_cast<std::uint8_t>(id >> (i * 8)));
  p.bytes = std::make_shared<const Bytes>(std::move(bytes));
  return p;
}
std::shared_ptr<const r::TriangleBuffer> square() {
  auto mesh = std::make_shared<r::TriangleBuffer>();
  mesh->positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
  mesh->normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  mesh->indices = {0, 1, 2, 0, 2, 3};
  return mesh;
}
struct Fixture {
  FrameState frame;
  Fixture() {
    frame.snapshot.event_count = frame.render.event_count = 44;
    object(1, "PxScene", 4);
    object(10, "PxRigidDynamic", 2);
    object(20, "PxRigidDynamic", 3);
    object(30, "PxArticulationLink", 5);
    object(60, "PxArticulation", 6);
    object(70, "PxAggregate", 7);
    for (auto id : {100u, 101u, 102u})
      object(id, "PxShape", 8);
    frame.snapshot.instances.at(60).properties["Links"] = references({30});
    frame.snapshot.instances.at(30).properties["Parent"] = references({60});
    frame.snapshot.instances.at(70).properties["Actors"] = references({10});
    frame.snapshot.instances.at(70).properties["Articulations"] =
        references({60});
    add(10, 100, {-1, 0, 0}, {220, 70, 50, 255});
    add(10, 102, {-1, 1, 0}, {220, 70, 50, 255});
    add(20, 100, {1, 0, -1}, {60, 90, 220, 255});
    add(30, 101, {0, 1, -2}, {60, 220, 90, 255});
  }
  void object(std::uint64_t id, const char *cls, std::uint64_t generation) {
    InstanceState value;
    value.id = id;
    value.generation = generation;
    value.class_name = {"physx3", cls};
    frame.snapshot.instances[id] = std::move(value);
  }
  void add(std::uint64_t actor, std::uint64_t shape, Vec3 position,
           Color color) {
    auto &a = frame.snapshot.instances.at(actor);
    a.properties["Scene"] = references({1});
    a.properties["GlobalPose"] =
        floats("PxTransform", {0, 0, 0, 1, position.x, position.y, position.z});
    a.properties["CMassLocalPose"] =
        floats("PxTransform", {0, 0, .38268343f, .92387953f, .2f, .1f, 0});
    a.properties["LinearVelocity"] = floats("PxVec3", {1, 0, 0});
    a.properties["AngularVelocity"] = floats("PxVec3", {0, 1, 0});
    r::ShapeInstance item;
    item.scene_id = 1;
    item.scene_generation = 4;
    item.actor_id = actor;
    item.actor_generation = a.generation;
    item.shape_id = shape;
    item.shape_generation = 8;
    item.geometry = r::Box{{1, 1, .01f}};
    item.actor.kind = actor == 30 ? r::ActorKind::articulation_link
                                  : r::ActorKind::rigid_dynamic;
    item.world_pose.position = position;
    item.world_bounds = r::Bounds{{position.x - 1, position.y - 1, position.z},
                                  {position.x + 1, position.y + 1, position.z}};
    frame.render.instances.push_back(item);
    frame.draws.push_back({square(), item.world_pose, item.picking_key(), color,
                           true, false, true});
  }
};
RenderOptions plain() {
  RenderOptions o;
  o.background_color = {0, 0, 0, 1};
  o.show_grid = o.show_axes = o.show_debug_points = o.show_debug_lines =
      o.show_debug_triangles = o.show_contacts = false;
  o.show_joint_frames = o.show_joint_limits = o.show_scene_queries =
      o.show_cloth = o.show_particles = false;
  return o;
}
void scopes(Renderer &renderer, OrbitCamera &camera) {
  Fixture f;
  auto &frame = f.frame;
  auto options = plain();
  options.show_solids = false;
  options.show_bounds = options.show_linear_velocity =
      options.show_angular_velocity = true;
  VisualizationSelection selection{{frame.draws[0].key.shape},
                                   frame.draws[0].key};
  const auto original =
      frame.snapshot.instances.at(10).properties.at("GlobalPose").raw_bytes();
  for (const auto scope :
       {VisualizationScope::none, VisualizationScope::all,
        VisualizationScope::selected, VisualizationScope::unselected}) {
    options.bounds_scope = options.velocity_scope = scope;
    renderer.render(frame, camera, options, selection);
    const auto stats = renderer.stats();
    const auto expected_bounds = scope == VisualizationScope::none       ? 0u
                                 : scope == VisualizationScope::all      ? 4u
                                 : scope == VisualizationScope::selected ? 1u
                                                                         : 3u;
    const auto expected_actors = scope == VisualizationScope::none       ? 0u
                                 : scope == VisualizationScope::all      ? 3u
                                 : scope == VisualizationScope::selected ? 1u
                                                                         : 3u;
    check(stats.body_bounds == expected_bounds &&
              stats.linear_velocity_vectors == expected_actors &&
              stats.angular_velocity_vectors == expected_actors,
          "actual GPU bounds/velocity scope uses exact shared-shape attachment "
          "and per-actor dedup");
  }
  options.bounds_scope = VisualizationScope::selected;
  options.velocity_scope = VisualizationScope::unselected;
  renderer.render(frame, camera, options, selection);
  check(renderer.stats().body_bounds == 1 &&
            renderer.stats().linear_velocity_vectors == 3,
        "bounds and velocity scopes are independent");
  options.show_bounds = options.show_linear_velocity =
      options.show_angular_velocity = false;
  options.show_actor_frames = options.show_shape_frames =
      options.show_mass_frames = true;
  options.coordinate_frame_scope = VisualizationScope::selected;
  renderer.render(frame, camera, options, selection);
  check(renderer.stats().actor_frames == 1 &&
            renderer.stats().shape_frames == 1 &&
            renderer.stats().mass_frames == 1,
        "coordinate frame selected attachment scope");
  selection.object = frame.draws[0].key.actor;
  renderer.render(frame, camera, options, selection);
  check(renderer.stats().actor_frames == 1 &&
            renderer.stats().shape_frames == 2 &&
            renderer.stats().mass_frames == 1,
        "explicit actor selects all its real attachments");
  selection.object = r::ObjectKey{60, 6};
  selection.attachment.reset();
  renderer.render(frame, camera, options, selection);
  check(renderer.stats().actor_frames == 1 &&
            renderer.stats().shape_frames == 1 &&
            renderer.stats().mass_frames == 1,
        "articulation owner follows actual Parent graph");
  selection.object = r::ObjectKey{70, 7};
  renderer.render(frame, camera, options, selection);
  check(renderer.stats().actor_frames == 2 &&
            renderer.stats().shape_frames == 3 &&
            renderer.stats().mass_frames == 2,
        "aggregate owner follows recorded rigids and articulation graph");
  selection.object = r::ObjectKey{10, 99};
  renderer.render(frame, camera, options, selection);
  check(renderer.stats().actor_frames == 0 &&
            renderer.coordinate_frames().diagnostics.empty(),
        "stale selected generation cannot retarget another owner");
  selection.object = r::ObjectKey{10, 2};
  frame.draws[0].visible = frame.draws[1].visible = false;
  renderer.render(frame, camera, options, selection);
  check(renderer.stats().actor_frames == 0 &&
            renderer.coordinate_frames().diagnostics.empty(),
        "hidden selected actor emits neither axes nor diagnostics");
  frame.draws[0].visible = frame.draws[1].visible = true;
  frame.snapshot.instances.at(10).properties.erase("CMassLocalPose");
  renderer.render(frame, camera, options, selection);
  check(renderer.stats().mass_frames == 0 &&
            !renderer.stats().coordinate_frames_complete &&
            renderer.stats().coordinate_frame_diagnostics == 1,
        "missing recorded COM gives explicit diagnostic rather than actor-axis "
        "fallback");
  check(
      frame.snapshot.instances.at(10).properties.at("GlobalPose").raw_bytes() ==
          original,
      "scope/axes never mutate source pose");
  options.show_actor_frames = options.show_shape_frames =
      options.show_mass_frames = false;
  options.show_scene_queries = true;
  r::SceneQuerySource q0, q1;
  q0.scene = q1.scene = {1, 4};
  q0.snapshot_event_count = q1.snapshot_event_count = 44;
  q1.sequence = 1;
  frame.scene_queries.lines = {{q0,
                                r::SceneQueryLineKind::requested_path,
                                {},
                                {-1, 0, 0},
                                {1, 0, 0},
                                0xffff0000u,
                                true},
                               {q1,
                                r::SceneQueryLineKind::requested_path,
                                {},
                                {-1, 1, 0},
                                {1, 1, 0},
                                0xff00ff00u,
                                true}};
  frame.scene_query_selection = q0;
  for (const auto scope :
       {VisualizationScope::none, VisualizationScope::all,
        VisualizationScope::selected, VisualizationScope::unselected}) {
    options.scene_query_scope = scope;
    renderer.render(frame, camera, options);
    const auto expected = scope == VisualizationScope::none  ? 0u
                          : scope == VisualizationScope::all ? 2u
                                                             : 1u;
    check(renderer.stats().query_lines == expected,
          "GPU scene-query scope uses actual selected query source");
  }
  std::cout << "GPU visualization source scopes/hidden/generation/owner graphs "
               "passed\n";
}
using Pixel = std::array<std::uint8_t, 4>;
Pixel center(const RgbaImage &image) {
  const auto at =
      (std::size_t(image.height / 2) * image.width + image.width / 2) * 4;
  return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2],
          image.pixels[at + 3]};
}
void transparency(Renderer &renderer, OrbitCamera &camera, ID3D11Device *device,
                  ID3D11DeviceContext *context) {
  Fixture f;
  auto &frame = f.frame;
  frame.draws.resize(2);
  frame.render.instances.resize(2);
  for (std::size_t i = 0; i < 2; ++i) {
    frame.draws[i].world_pose.position = {0, 0, i ? -1.f : 0.f};
    frame.draws[i].color = i ? Color{255, 0, 0, 255} : Color{0, 0, 255, 255};
    frame.render.instances[i].world_pose = frame.draws[i].world_pose;
    const float z = frame.draws[i].world_pose.position.z;
    frame.render.instances[i].world_bounds = r::Bounds{{-1, -1, z}, {1, 1, z}};
  }
  auto options = plain();
  options.transparent_scope = VisualizationScope::all;
  options.transparent_opacity = .5f;
  renderer.render(frame, camera, options);
  auto first = readTextureRgba8(device, context, renderer.texture());
  const auto blended = center(first);
  check(renderer.stats().transparent_draws == 2 && blended[0] > 10 &&
            blended[2] > blended[0] + 10 && blended[1] < 3,
        "real alpha composition contains back red and stronger front blue");
  std::reverse(frame.draws.begin(), frame.draws.end());
  renderer.render(frame, camera, options);
  auto reverse = readTextureRgba8(device, context, renderer.texture());
  check(first.pixels == reverse.pixels,
        "transparent back-to-front result is independent of attachment input "
        "order");
  auto &debug = frame.debug.lines.emplace_back();
  debug.position0 = {-.6f, 0, -.5f};
  debug.position1 = {.6f, 0, -.5f};
  debug.color0 = debug.color1 = 0xffffffffu;
  options.show_debug_lines = true;
  renderer.render(frame, camera, options);
  auto behind = readTextureRgba8(device, context, renderer.texture());
  bool white = false;
  for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x) {
      const auto at = (std::size_t(int(behind.height / 2) + y) * behind.width +
                       std::size_t(int(behind.width / 2) + x)) *
                      4;
      white |= behind.pixels[at] > 220 && behind.pixels[at + 1] > 220 &&
               behind.pixels[at + 2] > 220;
    }
  check(white, "transparent surfaces do not write depth and hide a "
               "behind-glass debug line");
  frame.debug.lines.clear();
  options.show_debug_lines = false;
  // An opaque selected occluder is rendered first, then rejects all glass
  // fragments behind it. The full image equals the same opaque object alone.
  auto green = frame.draws.front();
  green.key.actor = {20, 3};
  green.key.shape = {101, 8};
  green.world_pose.position = {0, 0, 1};
  green.color = {0, 255, 0, 255};
  auto green_shape = frame.render.instances.front();
  green_shape.actor_id = 20;
  green_shape.actor_generation = 3;
  green_shape.shape_id = 101;
  green_shape.world_pose = green.world_pose;
  green_shape.world_bounds = r::Bounds{{-1, -1, 1}, {1, 1, 1}};
  frame.draws.push_back(green);
  frame.render.instances.push_back(green_shape);
  options.transparent_scope = VisualizationScope::unselected;
  VisualizationSelection selected_green{green.key.actor, green.key};
  renderer.render(frame, camera, options, selected_green);
  auto occluded = readTextureRgba8(device, context, renderer.texture());
  auto single = frame;
  single.draws = {green};
  single.render.instances = {green_shape};
  renderer.render(single, camera, options, selected_green);
  auto opaque = readTextureRgba8(device, context, renderer.texture());
  check(occluded.pixels == opaque.pixels,
        "opaque depth occludes later transparent fragments and highlights");
  frame.draws.pop_back();
  frame.render.instances.pop_back();
  const auto front = std::find_if(
      frame.draws.begin(), frame.draws.end(),
      [](const auto &draw) { return draw.world_pose.position.z == 0; });
  check(front != frame.draws.end(), "front actual attachment");
  VisualizationSelection front_selection{front->key.shape, front->key};
  options.transparent_scope = VisualizationScope::selected;
  options.transparent_opacity = 0;
  const auto picks = visualization_pick_draws(frame, options, front_selection);
  check(picks.size() == 1 && picks[0].key.shape.id != front->key.shape.id,
        "zero-opacity selected attachment is excluded from actual pick "
        "candidates");
  auto hit = pick(picks, camera.screen_ray(320, 240, 640, 480));
  check(hit && hit->key == picks[0].key,
        "picking passes through invisible front surface to actual back "
        "attachment");
  renderer.render(frame, camera, options, front_selection);
  auto invisible = readTextureRgba8(device, context, renderer.texture());
  auto back = frame;
  back.draws = picks;
  renderer.render(back, camera, options, front_selection);
  check(invisible.pixels ==
            readTextureRgba8(device, context, renderer.texture()).pixels,
        "zero-opacity surface and selection wire do not leak GPU pixels");
  options.transparent_scope = VisualizationScope::all;
  check(visualization_pick_draws(frame, options).empty(),
        "all zero-opacity faces are unpickable");
  renderer.render(frame, camera, options);
  check(renderer.stats().triangles == 0,
        "zero opacity submits no invisible mesh/depth");
  std::cout << "WARP transparent sorting/opaque depth/read-only "
               "glass/opacity-zero picking passed\n";
}

struct Harness {
  Document doc;
  OrbitCamera camera;
  RenderOptions options;
  RenderStats stats;
  DisplayPanelState panel;
  float scale;
  ImGuiWindow *window = nullptr;
  ImVec2 last_click;
  explicit Harness(float s) : scale(s) {}
  void frame(std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
    UiPixelScope pixels(scale);
    auto &io = ImGui::GetIO();
    io.DisplaySize = {420 * scale, 700 * scale};
    io.DeltaTime = 1.f / 60;
    if (mouse)
      io.AddMousePosEvent(mouse->x, mouse->y);
    if (down)
      io.AddMouseButtonEvent(0, *down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Visualization controls", nullptr,
                 ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize);
    drawDisplaySettings(doc, camera, options, stats, panel);
    window = ImGui::GetCurrentWindow();
    check(window->DC.CursorMaxPos.x <= window->InnerRect.Max.x + 2 * scale,
          "visualization controls fit narrow logical width at each DPI");
    ImGui::End();
    ImGui::Render();
    check(panel.error.empty(), "visualization control action succeeds");
  }
  void key(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    frame();
    ImGui::GetIO().AddKeyEvent(key, false);
    frame();
  }
  ImVec2 focus(const char *label) {
    frame();
    const auto id = window->GetID(label);
    for (unsigned i = 0; i < 100; ++i) {
      const auto &g = *ImGui::GetCurrentContext();
      if (g.NavWindow == window && g.NavId == id) {
        frame();
        frame();
        const auto at = ImGui::WindowRectRelToAbs(window, window->NavRectRel[0])
                            .GetCenter();
        if (at.y > ImGui::GetIO().DisplaySize.y * .75f) {
          const ImVec2 inside{210 * scale, 350 * scale};
          frame(inside, false);
          ImGui::GetIO().AddMouseWheelEvent(0, -2);
          frame(inside, false);
          frame();
        }
        return ImGui::WindowRectRelToAbs(window, window->NavRectRel[0])
            .GetCenter();
      }
      key(ImGuiKey_Tab);
    }
    throw std::runtime_error(std::string("cannot focus ") + label);
  }
  void click(ImVec2 p) {
    last_click = p;
    frame(p, false);
    frame(p, true);
    frame(p, false);
    frame(p, false);
  }
  void click(const char *label) { click(focus(label)); }
  void choose(const char *label, int row) {
    click(label);
    frame();
    const auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
    if (!stack.Size || !stack.back().Window)
      throw std::runtime_error(
          std::string("actual combo did not open: ") + label +
          " click=" + std::to_string(last_click.y) +
          " scroll=" + std::to_string(window->Scroll.y) +
          " nav=" + std::to_string(window->NavRectRel[0].Min.y));
    auto p = stack.back().Window->DC.CursorStartPos;
    click({p.x + 10 * scale, p.y + ImGui::GetTextLineHeight() * .5f +
                                 row * ImGui::GetTextLineHeightWithSpacing()});
  }
};
void widgets(float scale) {
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  UiScale fonts(ImGui::GetStyle());
  fonts.reload_fonts(scale);
  Harness h(scale);
  h.frame();
  h.frame();
  h.choose("Bounds scope", 2);
  h.choose("Velocity scope", 3);
  h.choose("Scene query scope", 0);
  check(h.options.bounds_scope == VisualizationScope::selected &&
            h.options.velocity_scope == VisualizationScope::unselected &&
            h.options.scene_query_scope == VisualizationScope::none,
        "actual independent scope combo controls");
  h.click("Actor coordinate frames");
  h.click("Shape coordinate frames");
  h.click("Mass coordinate frames");
  h.choose("Coordinate frame scope", 2);
  check(h.options.show_actor_frames && h.options.show_shape_frames &&
            h.options.show_mass_frames &&
            h.options.coordinate_frame_scope == VisualizationScope::selected,
        "actual coordinate frame controls");
  h.choose("Transparent scope", 3);
  const auto opacity = h.focus("Opacity");
  // ImGui intentionally enters text input when a slider is reached by Tab.
  // Commit that input before testing the slider's actual mouse track.
  if (ImGui::TempInputIsActive(h.window->GetID("Opacity")))
    h.key(ImGuiKey_Enter);
  const auto rect =
      ImGui::WindowRectRelToAbs(h.window, h.window->NavRectRel[0]);
  h.click({rect.Min.x + 5 * scale, opacity.y});
  if (h.options.transparent_scope != VisualizationScope::unselected ||
      h.options.transparent_opacity >= .1f)
    throw std::runtime_error("actual opacity slider value=" +
                             std::to_string(h.options.transparent_opacity) +
                             " y=" + std::to_string(h.last_click.y) +
                             " scroll=" + std::to_string(h.window->Scroll.y));
  const auto point = h.focus("Point size (pixels)");
  if (ImGui::TempInputIsActive(h.window->GetID("Point size (pixels)")))
    h.key(ImGuiKey_Enter);
  const auto point_rect =
      ImGui::WindowRectRelToAbs(h.window, h.window->NavRectRel[0]);
  h.click({point_rect.Min.x + point_rect.GetWidth() * .75f, point.y});
  check(h.options.point_size > 30, "actual particle/debug point size slider");
  h.focus("Contact normal length");
  std::cout << "Actual scope/coordinate/opacity/point controls scale " << scale
            << " passed\n";
  ImGui::DestroyContext();
}
void real_gpu(Renderer &renderer, OrbitCamera &camera, ID3D11Device *device,
              ID3D11DeviceContext *context, const char *path) {
  Document doc;
  doc.open_file(path);
  check(doc.session() && !doc.session()->frames().empty(),
        "real native SDK frames");
  check(doc.set_scene(doc.session()->frames().front().scene_id) &&
            doc.set_sample_point(SamplePoint::after_fetch) && doc.set_frame(1),
        "real SDK explicit scene/fetch prefix");
  auto frame = doc.frame();
  const auto found =
      std::find_if(frame->render.instances.begin(),
                   frame->render.instances.end(), [](const auto &shape) {
                     return shape.actor.kind == r::ActorKind::articulation_link;
                   });
  check(found != frame->render.instances.end(),
        "real SDK articulation attachment");
  const auto key = found->picking_key();
  const auto raw = frame->snapshot.instance(key.actor.id)
                       ->property("GlobalPose")
                       ->raw_bytes();
  check(doc.select(key) && doc.fit_scene(camera, 4. / 3),
        "real SDK selection and fitting");
  auto options = plain();
  options.show_bounds = options.show_linear_velocity =
      options.show_actor_frames = options.show_shape_frames =
          options.show_mass_frames = true;
  options.bounds_scope = options.velocity_scope =
      options.coordinate_frame_scope = VisualizationScope::selected;
  options.transparent_scope = VisualizationScope::unselected;
  options.transparent_opacity = .15f;
  VisualizationSelection selection{doc.inspected_object(), doc.selected_key()};
  renderer.render(*doc.frame(), camera, options, selection);
  const auto stats = renderer.stats();
  check(stats.body_bounds == 1 && stats.linear_velocity_vectors == 1 &&
            stats.actor_frames == 1 && stats.shape_frames == 1 &&
            stats.mass_frames == 1 && stats.coordinate_frames_complete &&
            stats.transparent_draws > 0,
        "real SDK selected body/COM axes and unselected transparent pass");
  const auto image = readTextureRgba8(device, context, renderer.texture());
  std::size_t visible = 0;
  for (std::size_t i = 0; i < image.pixels.size(); i += 4)
    visible += image.pixels[i] || image.pixels[i + 1] || image.pixels[i + 2];
  check(visible > 100,
        "real SDK GPU output contains visible geometry and layers");
  doc.set_visible(key, false);
  renderer.render(*doc.frame(), camera, options, selection);
  check(renderer.stats().body_bounds == 0 &&
            renderer.stats().mass_frames == 0 &&
            renderer.coordinate_frames().diagnostics.empty(),
        "real SDK hidden attachment cannot leak bounds or COM diagnostics");
  const auto pick_draws =
      visualization_pick_draws(*doc.frame(), options, selection);
  check(std::none_of(pick_draws.begin(), pick_draws.end(),
                     [&](const auto &item) { return item.key == key; }),
        "real SDK hidden source excluded from pick adapter");
  check(doc.frame()
                ->snapshot.instance(key.actor.id)
                ->property("GlobalPose")
                ->raw_bytes() == raw,
        "real SDK view layers preserve original source pose bytes");
  std::cout << "Real SDK WARP selected/hidden/native COM/transparency output "
               "passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                      nullptr, 0, D3D11_SDK_VERSION, &device,
                                      &level, &context)),
          "actual WARP device");
    Renderer renderer(device.Get(), context.Get());
    renderer.resize(640, 480);
    OrbitCamera camera;
    camera.yaw = camera.pitch = 0;
    camera.distance = 5;
    scopes(renderer, camera);
    transparency(renderer, camera, device.Get(), context.Get());
    if (argc == 2)
      real_gpu(renderer, camera, device.Get(), context.Get(), argv[1]);
    else
      check(argc == 1,
            "usage: visualization_layers_tests [articulation_capture.pxd2]");
    for (float scale : {1.f, 1.5f, 2.f})
      widgets(scale);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
