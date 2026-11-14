// Real model state and real Dear ImGui widgets. No PhysX/Win32/GPU required.
#include "imgui.h"
#include "imgui_internal.h"
#include "panels.hpp"
#include "scene_tools.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
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
Bytes floats(std::initializer_list<float> values) {
  Bytes b;
  for (float f : values) {
    std::uint32_t n;
    std::memcpy(&n, &f, 4);
    for (int j = 0; j < 4; ++j)
      b.push_back(static_cast<std::uint8_t>(n >> (8 * j)));
  }
  return b;
}
Bytes ids(std::initializer_list<std::uint64_t> values) {
  Bytes b;
  for (auto n : values)
    for (int j = 0; j < 8; ++j)
      b.push_back(static_cast<std::uint8_t>(n >> (8 * j)));
  return b;
}
struct Fixture {
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  void emit(Payload p) {
    events.push_back({1, events.size(), events.size() * 9, std::move(p)});
  }
  std::uint32_t str(const std::string &s) {
    auto f = strings.find(s);
    if (f != strings.end())
      return f->second;
    auto h = static_cast<std::uint32_t>(strings.size() + 1);
    strings[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *n) {
    auto a = str("physx3"), b = str(n);
    return {a, b};
  }
  void prop(const char *c, const char *p, const char *t, bool array = false) {
    auto cls = type(c), dt = type(t);
    auto name = str(p);
    emit(CreateProperty{
        cls, name, 0, dt, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void create(const char *c, std::uint64_t id) {
    emit(CreateInstance{type(c), id});
  }
  void value(std::uint64_t id, const char *p, const char *t, Bytes b,
             std::uint32_t count = 1) {
    auto name = str(p);
    auto dt = type(t);
    emit(SetPropertyValue{id, name, dt, count, std::move(b)});
  }
  void refs(std::uint64_t id, const char *p,
            std::initializer_list<std::uint64_t> v) {
    value(id, p, "VoidPtr", ids(v), static_cast<std::uint32_t>(v.size()));
  }
  void pose(std::uint64_t id, Vec3 p, const char *prop = "GlobalPose") {
    value(id, prop, "PxTransform", floats({0, 0, 0, 1, p.x, p.y, p.z}));
  }
  void name(std::uint64_t id, const char *n) {
    Bytes b(8);
    b.insert(b.end(), n, n + std::strlen(n) + 1);
    value(id, "Name", "String", std::move(b));
  }
  void actor(std::uint64_t id, std::uint64_t scene, Vec3 p, std::uint64_t shape,
             bool dynamic = false, bool kin = false, bool sleep = false) {
    create(dynamic ? "PxRigidDynamic" : "PxRigidStatic", id);
    refs(id, "Scene", {scene});
    refs(id, "Shapes", {shape});
    pose(id, p);
    name(id, ("actor_" + std::to_string(id)).c_str());
    if (dynamic) {
      value(id, "RigidBodyFlags", "PvdU8",
            Bytes{static_cast<std::uint8_t>(kin)});
      value(id, "IsSleeping", "PvdBool",
            Bytes{static_cast<std::uint8_t>(sleep)});
    }
  }
  void mark(bool begin, std::uint64_t scene = 1) {
    auto h = str("frame");
    if (begin)
      emit(BeginSection{scene, h, events.size()});
    else
      emit(EndSection{scene, h, events.size()});
  }
  Fixture() {
    for (const char *c :
         {"PxScene", "PxRigidStatic", "PxRigidDynamic", "PxShape",
          "PxBoxGeometry", "PxMaterial", "PxD6Joint", "PxCloth"})
      emit(CreateClass{type(c)});
    for (const char *c : {"PxRigidStatic", "PxRigidDynamic"}) {
      prop(c, "Scene", "ObjectRef");
      prop(c, "Shapes", "ObjectRef", true);
      prop(c, "GlobalPose", "PxTransform");
      prop(c, "Name", "StringHandle");
    }
    prop("PxRigidDynamic", "RigidBodyFlags", "PvdU8");
    prop("PxRigidDynamic", "IsSleeping", "PvdBool");
    prop("PxShape", "Geometry", "ObjectRef");
    prop("PxShape", "LocalPose", "PxTransform");
    prop("PxShape", "Flags", "PvdU8");
    prop("PxShape", "Materials", "ObjectRef", true);
    prop("PxBoxGeometry", "HalfExtents", "PxVec3");
    create("PxScene", 1);
    create("PxScene", 2);
    create("PxBoxGeometry", 200);
    value(200, "HalfExtents", "PxVec3", floats({.5f, .5f, .5f}));
    create("PxMaterial", 300);
    create("PxD6Joint", 301);
    create("PxCloth", 302);
    for (std::uint64_t i = 100; i <= 101; ++i) {
      create("PxShape", i);
      refs(i, "Geometry", {200});
      pose(i, {}, "LocalPose");
      value(i, "Flags", "PvdU8",
            Bytes{static_cast<std::uint8_t>(i == 100 ? 11 : 6)});
      refs(i, "Materials", {300});
    }
    actor(10, 1, {}, 100);
    actor(11, 1, {0, 0, -4}, 100, true);
    actor(12, 1, {4, 0, 0}, 101, true, true);
    actor(13, 1, {8, 0, 0}, 100, true, false, true);
    actor(21, 2, {20, 0, 0}, 100);
    emit(SetCamera{"rolled", {0, 0, 5}, {1, 1, 0}, {0, 0, 0}});
    emit(SetCamera{"Z-up", {4, 0, 2}, {0, 0, 1}, {0, 0, 0}});
    emit(SetCamera{"invalid", {0, 0, 5}, {0, 0, 1}, {0, 0, 0}});
    mark(true, 2);
    mark(false, 2);
    mark(true);
    mark(false);
    mark(true);
    pose(10, {2, 0, 0});
    emit(SetCamera{"rolled", {2, 1, 7}, {1, 1, 0}, {1, 0, 0}});
    emit(DestroyInstance{11});
    mark(false);
    mark(true);
    actor(11, 1, {0, 0, -4}, 100, true);
    emit(DestroyInstance{10});
    mark(false);
    // Many same-class resources exercise class pagination/virtualization.
    for (std::uint64_t i = 1000; i < 1600; ++i)
      create("PxMaterial", i);
    mark(true);
    mark(false);
  }
  void load(Document &doc) {
    StreamHeader h;
    h.stream_flags = 1;
    doc.adopt(std::make_unique<Session>(h, events), "scene tools fixture");
    doc.set_scene(1);
    doc.set_frame(0);
  }
};
PickingKey key(const Document &doc, std::uint64_t actor) {
  for (const auto &s : doc.frame()->render.instances)
    if (s.actor_id == actor)
      return s.picking_key();
  throw std::runtime_error("fixture draw missing");
}
std::size_t visible(const Document &doc) {
  std::size_t n = 0;
  for (const auto &d : doc.frame()->draws)
    n += d.visible;
  return n;
}
std::array<double, 4> apply(const Mat4 &m, Vec3 p) {
  std::array<double, 4> v{};
  for (int r = 0; r < 4; ++r)
    v[r] = double(m.values[r]) * p.x + double(m.values[4 + r]) * p.y +
           double(m.values[8 + r]) * p.z + m.values[12 + r];
  return v;
}
void view_model() {
  Fixture f;
  Document doc;
  f.load(doc);
  const auto a = key(doc, 10), b = key(doc, 11);
  check(a.shape == b.shape, "synthetic shapes are not shared");
  auto before = doc.frame();
  auto bytes = before->snapshot.instance(10)->property("GlobalPose")->bytes;
  auto mesh = before->draws[0].mesh;
  doc.set_visible(a, false);
  check(!doc.visible(a) && doc.visible(b) && visible(doc) == 3,
        "shared shape attachment hide leaked to another actor");
  check(before->draws[0].visible &&
            before->snapshot.instance(10)->property("GlobalPose")->bytes ==
                bytes &&
            doc.frame()->snapshot.instance(10)->property("GlobalPose")->bytes ==
                bytes,
        "view state changed source snapshot or previously published frame");
  check(doc.frame()->draws[0].mesh == mesh,
        "visibility command retessellated geometry");
  const Ray ray{{0, 0, 5}, {0, 0, -1}};
  auto hit = pick(doc.frame()->draws, ray);
  check(hit && hit->key == b,
        "hidden foreground attachment remained CPU-pickable");
  doc.set_actor_visible(b.scene, b.actor, false);
  check(!pick(doc.frame()->draws, ray), "hidden actor remained CPU-pickable");
  doc.set_frame(1);
  check(!doc.visible(a), "seek lost stable attachment hide state");
  doc.set_frame(2);
  const auto reused = key(doc, 11);
  check(reused.actor.generation == 1 && doc.visible(reused),
        "hide rule leaked into reused actor generation");
  doc.set_frame(0);
  check(!doc.visible(b), "reverse seek lost old generation hide rule");
  doc.show_all();
  check(visible(doc) == 4, "restore all did not recover all attachments");
  doc.isolate(a);
  check(doc.isolated() && visible(doc) == 1 && doc.visible(a),
        "isolate did not retain exact attachment");
  doc.show_all();
  doc.isolate_actor(b.scene, b.actor);
  check(visible(doc) == 1 && doc.visible(b),
        "actor isolate selected another attachment");
  doc.show_all();
  for (int mode = 0; mode < 5; ++mode) {
    auto v = doc.view_options();
    v.show_static = mode != 0;
    v.show_dynamic = mode != 1;
    v.show_kinematic = mode != 2;
    v.show_sleeping = mode != 3;
    v.show_trigger = mode != 4;
    doc.set_view_options(v);
    check(visible(doc) == static_cast<std::size_t>(mode == 1 ? 2 : 3),
          "actor/status category visibility filter mismatch");
    doc.show_all();
  }
  auto v = doc.view_options();
  v.color_mode = ColorMode::uniform;
  v.uniform_color = {1, 2, 3, 4};
  doc.set_view_options(v);
  for (const auto &d : doc.frame()->draws)
    check(d.color.r == 1 && d.color.g == 2 && d.color.b == 3 && d.color.a == 4,
          "uniform view color did not apply");
  check(!doc.frame()->render.instances[0].color &&
            !before->render.instances[0].color,
        "view color mutated extracted source attributes");
  v.color_mode = ColorMode::actor_identity;
  doc.set_view_options(v);
  auto color = doc.frame()->draws[0].color;
  doc.set_frame(1);
  check(doc.frame()->draws[0].color.r == color.r &&
            doc.frame()->draws[0].color.g == color.g &&
            doc.frame()->draws[0].color.b == color.b,
        "actor color changed while seeking within a lifetime");
  doc.set_frame(0);
  auto groups = doc.object_groups();
  for (const char *cls :
       {"PxD6Joint", "PxMaterial", "PxCloth", "PxBoxGeometry"})
    check(std::any_of(groups.begin(), groups.end(),
                      [&](const auto &g) {
                        return g.class_name.name == cls && !g.objects.empty();
                      }),
          "non-actor object category missing");
  doc.inspect(300);
  check(doc.inspected_object()->id == 300,
        "global resource cannot be inspected");
  doc.set_visible(a, false);
  f.load(doc);
  check(visible(doc) == 4 && !doc.isolated() &&
            doc.view_options().color_mode == ColorMode::recorded_status,
        "replacement capture retained prior view state");
  std::cout << "scene view state/shared attachment/lifecycle/picking passed\n";
}
void cameras_and_tracking() {
  Fixture f;
  Document doc;
  f.load(doc);
  OrbitCamera camera;
  camera.target = {5, 3, 1};
  const auto original = camera.target;
  const auto dist = camera.distance;
  SceneToolState state;
  doc.inspect(10);
  check(trackSelected(doc, camera, state), "actor tracking could not start");
  doc.set_frame(1);
  check(updateTrackedCamera(doc, camera, state),
        "tracked camera did not advance with actor");
  near(camera.target, {original.x + 2, original.y, original.z},
       "tracking destroyed camera pan offset");
  near(camera.distance, dist, "tracking altered camera zoom");
  doc.set_frame(0);
  check(updateTrackedCamera(doc, camera, state),
        "reverse tracking did not move");
  near(camera.target, original, "reverse track failed to recover target");
  doc.set_frame(2);
  check(!updateTrackedCamera(doc, camera, state) && !state.tracking(),
        "destroyed tracked lifetime did not stop");
  doc.set_frame(0);
  doc.select(key(doc, 11));
  check(trackSelected(doc, camera, state),
        "shared shape attachment tracking did not start");
  doc.set_frame(2);
  check(!updateTrackedCamera(doc, camera, state) && !state.tracking(),
        "shape tracking crossed reused actor generation");
  doc.set_frame(0);
  check(applyRecordedCamera(doc, "rolled", camera, state),
        "rolled recorded camera rejected");
  near(camera.position(), {0, 0, 5}, "recorded camera eye changed");
  const auto m = camera.view_matrix();
  const auto root = std::sqrt(.5);
  near(m.values[0], root, "recorded camera right x");
  near(m.values[4], -root, "recorded camera right y (roll lost)");
  near(m.values[1], root, "recorded camera up x (roll lost)");
  near(m.values[5], root, "recorded camera up y");
  const auto top = camera.screen_ray(400, 0, 800, 600);
  check(top.direction.x > 0 && top.direction.y > 0 && top.direction.z < 0,
        "CPU ray ignored recorded camera roll");
  camera.orbit(.2, .1);
  camera.pan(10, 4, 600);
  camera.zoom(3);
  check(std::isfinite(camera.view_projection(1.7).values[0]),
        "recorded arbitrary-up orbit/pan/zoom invalid");
  check(applyRecordedCamera(doc, "Z-up", camera, state),
        "Z-up recorded camera rejected");
  near(camera.position(), {4, 0, 2}, "Z-up eye changed");
  const auto target = apply(camera.view_matrix(), {0, 0, 0});
  near(target[0], 0, "Z-up camera horizontal target");
  near(target[1], 0, "Z-up camera vertical target");
  check(target[2] < 0, "Z-up RH target depth invalid");
  render::Bounds box{{-1, -2, -3}, {2, 3, 4}};
  camera.fit(box, 1.7);
  const auto vp = camera.view_projection(1.7);
  for (float x : {box.minimum.x, box.maximum.x})
    for (float y : {box.minimum.y, box.maximum.y})
      for (float z : {box.minimum.z, box.maximum.z}) {
        auto p = apply(vp, {x, y, z});
        check(p[3] > 0 && std::abs(p[0] / p[3]) <= 1 &&
                  std::abs(p[1] / p[3]) <= 1 && p[2] / p[3] >= 0 &&
                  p[2] / p[3] <= 1,
              "arbitrary-up fit dropped a corner outside frustum");
      }
  camera.set_look_at({{0, 0, 5}, {1, 1, 1}, {0, 0, 0}});
  const auto tilted = camera.view_matrix();
  near(tilted.values[0], root, "non-orthogonal source up right x");
  near(tilted.values[4], -root, "non-orthogonal source up right y");
  near(camera.position(), {0, 0, 5}, "non-orthogonal up changed eye");
  const auto eye = camera.position();
  check(!applyRecordedCamera(doc, "invalid", camera, state),
        "collinear camera up was accepted");
  near(camera.position(), eye,
       "invalid recorded camera partially changed view");
  doc.inspect(10);
  trackSelected(doc, camera, state);
  f.load(doc);
  check(!updateTrackedCamera(doc, camera, state) && !state.tracking(),
        "capture replacement retained tracking");
  std::cout
      << "recorded arbitrary-up/rolled camera and lifetime tracking passed\n";
}
struct Output {
  std::string text;
  float overflow = 0;
};
Output draw(Document &doc, OrbitCamera &camera, SceneToolState &scene,
            PanelState &panel, bool hierarchy, float width,
            std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
  auto &io = ImGui::GetIO();
  io.DisplaySize = {width, 1000};
  io.DeltaTime = 1.f / 60;
  if (mouse)
    io.AddMousePosEvent(mouse->x, mouse->y);
  if (down)
    io.AddMouseButtonEvent(0, *down);
  updateTrackedCamera(doc, camera, scene);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({width, 920});
  ImGui::Begin("Scene tools test", nullptr,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoTitleBar |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::LogToBuffer();
  if (hierarchy)
    drawHierarchy(doc, panel);
  else
    drawSceneTools(doc, camera, scene);
  Output out;
  out.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  auto *w = ImGui::GetCurrentWindow();
  out.overflow = std::max(0.f, w->DC.CursorMaxPos.x - w->InnerRect.Max.x);
  ImGui::End();
  ImGui::Render();
  return out;
}
void widgets(float width) {
  Fixture f;
  Document doc;
  f.load(doc);
  OrbitCamera camera;
  SceneToolState scene;
  PanelState panel;
  draw(doc, camera, scene, panel, false, width);
  const auto initial = draw(doc, camera, scene, panel, false, width);
  check(initial.text.find("4 / 4 shapes visible") != std::string::npos,
        "scene controls did not display actual visibility");
  // Actual first category checkbox toggles the underlying model, not a mock.
  const auto &style = ImGui::GetStyle();
  ImVec2 check_at{style.WindowPadding.x + 7,
                  style.WindowPadding.y +
                      ImGui::GetTextLineHeightWithSpacing() +
                      ImGui::GetFrameHeight() * .5f};
  draw(doc, camera, scene, panel, false, width, check_at, false);
  draw(doc, camera, scene, panel, false, width, check_at, true);
  draw(doc, camera, scene, panel, false, width, check_at, false);
  check(!doc.view_options().show_static && visible(doc) == 3,
        "actual Static checkbox did not update view model");
  doc.show_all();
  doc.set_frame(3);
  std::snprintf(panel.hierarchy_search.data(), panel.hierarchy_search.size(),
                "PxMaterial");
  const auto resources = draw(doc, camera, scene, panel, true, width);
  check(resources.text.find("PxMaterial") != std::string::npos &&
            panel.object_count == 601,
        "class tree search omitted captured resources");
  check(panel.object_visible_rows > 0 && panel.object_visible_rows <= 128,
        "resource rows were not paged/virtualized");
  doc.inspect(301);
  panel.hierarchy_search[0] = 0;
  panel.reveal_object = doc.inspected_object();
  const auto revealed = draw(doc, camera, scene, panel, true, width);
  check(revealed.text.find("PxD6Joint") != std::string::npos &&
            doc.inspected_object()->id == 301,
        "reference/object navigation lost non-shape inspection");
  check(initial.overflow <= 1 && resources.overflow <= 1,
        "scene controls or resource tree overflow available width");
  std::cout << "scene widgets width=" << width
            << " virtual rows=" << panel.object_visible_rows << " passed\n";
}
void recorded_camera_widgets(float width) {
  Fixture f;
  Document doc;
  f.load(doc);
  OrbitCamera camera;
  SceneToolState scene;
  PanelState panel;
  auto tick = [&](std::optional<ImVec2> pos = {},
                  std::optional<bool> down = {}) {
    return draw(doc, camera, scene, panel, false, width, pos, down);
  };
  auto click = [&](SceneToolRect r) {
    check(r.width > 0 && r.height > 0, "camera widget actual rect missing");
    const ImVec2 p{r.x + r.width * .5f, r.y + r.height * .5f};
    tick(p, false);tick(p, true);tick(p, false);
  };
  auto choose = [&] {
    tick();tick();
    click(scene.recorded_combo_rect);
    tick();
    const auto found=scene.recorded_choice_rects.find("rolled");
    check(found!=scene.recorded_choice_rects.end(),"actual camera combo did not open");
    const auto rect=found->second;
    click(rect);
    if (!scene.recorded_follow.enabled)
      std::cerr<<"camera choose width="<<width<<" row="<<rect.x<<','<<rect.y
               <<','<<rect.width<<','<<rect.height<<" name="<<scene.recorded_camera
               <<" message="<<scene.message<<" error="<<scene.error<<'\n';
    check(scene.recorded_follow.enabled&&!scene.tracking_object(),
          "actual recorded camera choice did not start exclusive Clip follow");
  };
  choose();
  near(camera.position(),{0,0,5},"camera combo used unrelated sample");
  doc.set_frame(1);tick();
  near(camera.position(),{2,1,7},"UI-selected Clip camera did not follow seek");
  tick();click(scene.free_camera_rect);
  check(!scene.recorded_follow.enabled,"actual Free camera did not release follow");
  doc.set_frame(0);tick();
  near(camera.position(),{2,1,7},"released camera continued following");
  click(scene.apply_camera_rect);
  check(!scene.tracking(),"actual Apply once enabled persistent tracking");
  near(camera.position(),{0,0,5},"actual Apply once did not apply current frame");
  doc.set_frame(1);tick();near(camera.position(),{0,0,5},"one-shot UI became follow");
  doc.set_frame(0);doc.inspect(10);
  check(trackSelected(doc,camera,scene),"object tracking test setup");
  choose();check(!scene.tracking_object(),"Clip choice retained selected-object tracking");
  check(trackSelected(doc,camera,scene)&&!scene.recorded_follow.enabled,
        "Track selected retained Clip camera follow");
  choose();f.load(doc);const auto source_changed=tick();
  check(!scene.recorded_follow.enabled&&scene.recorded_follow.status==RecordedCameraStatus::source_changed&&
            source_changed.text.find("source changed")!=std::string::npos,
        "source replacement silently lost follow diagnostic");
  check(source_changed.overflow<=1,"recorded camera controls overflow available width");
  std::cout<<"recorded camera widgets width="<<width<<" combo/seek/free/apply/source passed\n";
}
void real(const char *path) {
  Document doc;
  doc.open_file(path);
  check(!doc.frame()->draws.empty(), "real fixture lacks shapes");
  auto key = doc.frame()->draws.front().key;
  auto count = visible(doc);
  doc.set_visible(key, false);
  check(visible(doc) + 1 == count && !doc.visible(key),
        "real SDK attachment hide failed");
  doc.show_all();
  doc.select(key);
  OrbitCamera c;
  SceneToolState s;
  check(trackSelected(doc, c, s), "real SDK selected shape cannot track");
  if (doc.timeline().size() > 1) {
    doc.set_frame(1);
    updateTrackedCamera(doc, c, s);
  }
  check(std::isfinite(c.view_matrix().values[0]),
        "real tracking camera invalid");
  std::cout << "real scene tools " << count << " geometry instances / "
            << doc.object_groups().size() << " classes passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    view_model();
    cameras_and_tracking();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char *b;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&b, &w, &h);
    widgets(1060);
    recorded_camera_widgets(1060);
    widgets(420);
    recorded_camera_widgets(420);
    if (argc > 1)
      real(argv[1]);
    ImGui::DestroyContext();
    std::cout << "scene tools tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
