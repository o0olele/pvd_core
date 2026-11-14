#include "deformable_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
namespace d = pvd::deformables;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void near(float a, float b, const char *message) {
  check(std::isfinite(a) && std::abs(a - b) < 1e-4f, message);
}
void near(Vec3 a, Vec3 b, const char *message) {
  near(a.x, b.x, message);
  near(a.y, b.y, message);
  near(a.z, b.z, message);
}
Bytes ints(std::initializer_list<std::uint64_t> values, std::size_t width = 4) {
  Bytes out;
  for (auto n : values)
    for (std::size_t i = 0; i < width; ++i)
      out.push_back(static_cast<std::uint8_t>(n >> (i * 8)));
  return out;
}
Bytes floats(std::initializer_list<float> values) {
  Bytes out;
  for (auto f : values) {
    std::uint32_t n;
    std::memcpy(&n, &f, 4);
    auto field = ints({n});
    out.insert(out.end(), field.begin(), field.end());
  }
  return out;
}
struct Fixture {
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  void emit(Payload payload) {
    events.push_back({1, events.size(), events.size() * 9, std::move(payload)});
  }
  std::uint32_t str(const std::string &text) {
    auto found = strings.find(text);
    if (found != strings.end())
      return found->second;
    auto handle = static_cast<std::uint32_t>(strings.size() + 1);
    strings.emplace(text, handle);
    emit(StringHandleEvent{text, handle});
    return handle;
  }
  WireName type(const char *n) {
    auto ns = str("physx3"), name = str(n);
    return {ns, name};
  }
  void prop(const char *c, const char *p, const char *t, bool array = false) {
    auto cls = type(c), datatype = type(t);
    auto name = str(p);
    emit(CreateProperty{
        cls, name, 0, datatype, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void create(const char *c, std::uint64_t id) {
    emit(CreateInstance{type(c), id});
  }
  void value(std::uint64_t id, const char *p, const char *t, Bytes bytes,
             std::uint32_t count = 1) {
    auto name = str(p);
    auto datatype = type(t);
    emit(SetPropertyValue{id, name, datatype, count, std::move(bytes)});
  }
  void u(std::uint64_t id, const char *p, std::uint64_t n) {
    value(id, p, "PvdU32", ints({n}));
  }
  void ref(std::uint64_t id, const char *p, std::uint64_t target) {
    value(id, p, "VoidPtr", ints({target}, 8));
  }
  void name(std::uint64_t id, const char *text) {
    Bytes bytes(8);
    bytes.insert(bytes.end(), text, text + std::strlen(text) + 1);
    value(id, "Name", "String", std::move(bytes));
  }
  void pose(std::uint64_t id, float x, bool rotate = true) {
    value(id, "GlobalPose", "PxTransform",
          floats({0, 0, rotate ? .70710677f : 0, rotate ? .70710677f : 1, x, 0,
                  0}));
  }
  void begin(std::uint64_t scene) {
    emit(BeginSection{scene, str("frame"), events.size()});
  }
  void end(std::uint64_t scene) {
    emit(EndSection{scene, str("frame"), events.size()});
  }
  void cloth(std::uint64_t id, std::uint64_t scene, float x, const char *text) {
    create("PxCloth", id);
    ref(id, "Scene", scene);
    ref(id, "Fabric", 100);
    u(id, "NbParticles", 4);
    u(id, "ClothFlags", 0);
    u(id, "ActorFlags", 0);
    value(id, "IsSleeping", "PvdBool", {0});
    name(id, text);
    pose(id, x);
    value(id, "ParticleBuffer", "PxClothParticle",
          floats({0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1}), 4);
  }
  void particles(std::uint64_t scene) {
    create("PxParticleSystem", 30);
    ref(30, "Scene", scene);
    name(30, "Sparse cloud");
    u(30, "ActorFlags", 0);
    u(30, "ParticleBaseFlags", 4);
    u(30, "ParticleReadDataFlags", 11);
    u(30, "MaxParticles", 96);
  }
  void data(unsigned version) {
    if (version == 0) {
      u(30, "NbParticles", 4);
      u(30, "ValidParticleRange", 33);
      value(30, "ValidParticleBitmap", "PvdU32", ints({0x80000081u, 1}), 2);
      value(30, "Positions", "PxVec3",
            floats({0, 0, 0, 2, 0, 0, 4, 0, 0, 6, 0, 0}), 4);
    } else if (version == 1) {
      u(30, "NbParticles", 3);
      u(30, "ValidParticleRange", 33);
      value(30, "ValidParticleBitmap", "PvdU32", ints({0x80000001u, 1}), 2);
      value(30, "Positions", "PxVec3", floats({0, 0, 0, 4, 0, 0, 6, 0, 0}), 3);
    } else if (version == 2) {
      u(30, "NbParticles", 0);
      u(30, "ValidParticleRange", 0);
      return;
    } else if (version == 3) {
      u(30, "NbParticles", 2);
      u(30, "ValidParticleRange", 32);
      value(30, "ValidParticleBitmap", "PvdU32", ints({0x80000080u, 0}), 2);
      value(30, "Positions", "PxVec3", floats({8, 0, 0, 4, 0, 0}), 2);
    } else {
      u(30, "NbParticles", 2);
      u(30, "ValidParticleRange", 64);
      value(30, "ValidParticleBitmap", "PvdU32", ints({4, 0x80000000u, 0}), 3);
      value(30, "Positions", "PxVec3", floats({3, 0, 0, 9, 0, 0}), 2);
    }
    const unsigned count = version == 0 ? 4 : version == 1 ? 3 : 2;
    Bytes velocities, flags;
    for (unsigned i = 0; i < count; ++i) {
      auto v = floats({0, 1, 0});
      velocities.insert(velocities.end(), v.begin(), v.end());
      auto f = ints({1}, 2);
      flags.insert(flags.end(), f.begin(), f.end());
    }
    value(30, "Velocities", "PxVec3", std::move(velocities), count);
    value(30, "Flags", "PvdU16", std::move(flags), count);
  }
  Fixture() {
    for (const char *n :
         {"PxScene", "PxActor", "PxCloth", "PxClothFabric", "PxClothParticle",
          "PxClothFabricPhase", "PxParticleBase", "PxParticleSystem",
          "PxRigidStatic", "PxShape", "PxBoxGeometry"})
      emit(CreateClass{type(n)});
    emit(DeriveClass{type("PxActor"), type("PxCloth")});
    emit(DeriveClass{type("PxActor"), type("PxParticleBase")});
    emit(DeriveClass{type("PxParticleBase"), type("PxParticleSystem")});
    prop("PxActor", "Name", "StringHandle");
    prop("PxActor", "Scene", "ObjectRef");
    prop("PxActor", "ActorFlags", "PvdU32");
    prop("PxCloth", "Fabric", "ObjectRef");
    prop("PxCloth", "NbParticles", "PvdU32");
    prop("PxCloth", "ClothFlags", "PvdU32");
    prop("PxCloth", "IsSleeping", "PvdBool");
    prop("PxCloth", "GlobalPose", "PxTransform");
    prop("PxCloth", "ParticleBuffer", "PxClothParticle", true);
    prop("PxClothParticle", "Pos", "PxVec3");
    prop("PxClothParticle", "InvWeight", "PvdF32");
    prop("PxClothFabricPhase", "PhaseType", "PvdU32");
    prop("PxClothFabricPhase", "SetIndex", "PvdU32");
    prop("PxClothFabric", "NbParticles", "PvdU32");
    prop("PxClothFabric", "ParticleIndices", "PvdU32", true);
    prop("PxClothFabric", "Sets", "PvdU32", true);
    prop("PxClothFabric", "Restvalues", "PvdF32", true);
    prop("PxClothFabric", "Phases", "PxClothFabricPhase", true);
    for (const char *n : {"ParticleBaseFlags", "ParticleReadDataFlags",
                          "MaxParticles", "NbParticles", "ValidParticleRange"})
      prop("PxParticleBase", n, "PvdU32");
    prop("PxParticleBase", "ValidParticleBitmap", "PvdU32", true);
    prop("PxParticleBase", "Positions", "PxVec3", true);
    prop("PxParticleBase", "Velocities", "PxVec3", true);
    prop("PxParticleBase", "Flags", "PvdU16", true);
    prop("PxRigidStatic", "Scene", "ObjectRef");
    prop("PxRigidStatic", "Shapes", "ObjectRef", true);
    prop("PxRigidStatic", "GlobalPose", "PxTransform");
    prop("PxShape", "Geometry", "ObjectRef");
    prop("PxShape", "LocalPose", "PxTransform");
    prop("PxShape", "Flags", "PvdU8");
    prop("PxBoxGeometry", "HalfExtents", "PxVec3");
    create("PxScene", 1);
    create("PxScene", 2);
    create("PxClothFabric", 100);
    u(100, "NbParticles", 4);
    value(100, "ParticleIndices", "PvdU32", ints({0, 1, 1, 2, 2, 3}), 6);
    value(100, "Sets", "PvdU32", ints({1, 3}), 2);
    value(100, "Restvalues", "PvdF32", floats({1, 1.41421356f, 1.7320508f}), 3);
    value(100, "Phases", "PxClothFabricPhase", ints({1, 0, 4, 1}), 2);
    cloth(10, 1, 10, "Pinned native cloth");
    cloth(20, 2, 100, "Other-scene cloth");
    particles(1);
    data(0);
    create("PxBoxGeometry", 401);
    value(401, "HalfExtents", "PxVec3", floats({.5f, .5f, .5f}));
    create("PxShape", 400);
    ref(400, "Geometry", 401);
    value(400, "LocalPose", "PxTransform", floats({0, 0, 0, 1, 0, 0, 0}));
    value(400, "Flags", "PvdU8", {11});
    create("PxRigidStatic", 40);
    ref(40, "Scene", 1);
    value(40, "Shapes", "VoidPtr", ints({400}, 8));
    pose(40, -4, false);
    begin(2);
    end(2);
    begin(1);
    end(1);
    begin(1);
    pose(10, 11);
    data(1);
    end(1);
    begin(1);
    data(2);
    value(10, "IsSleeping", "PvdBool", {1});
    end(1);
    begin(1);
    data(3);
    pose(10, 12);
    value(10, "IsSleeping", "PvdBool", {0});
    end(1);
    begin(1);
    emit(DestroyInstance{30});
    particles(1);
    data(4);
    emit(DestroyInstance{10});
    cloth(10, 1, 13, "Recreated native cloth");
    end(1);
  }
  void load(Document &doc) {
    StreamHeader h;
    h.stream_flags = 1;
    doc.adopt(std::make_unique<Session>(h, events),
              "native deformables synthetic");
    doc.set_scene(1);
  }
};
const d::ParticleCloud &cloud(const Document &doc) {
  check(doc.frame()->deformables.particle_clouds.size() == 1,
        "one native cloud in selected scene");
  return doc.frame()->deformables.particle_clouds.front();
}
void model() {
  Fixture fixture;
  Document doc;
  fixture.load(doc);
  check(doc.timeline().size() == 5, "native timeline size");
  check(doc.frame()->deformables.cloths.size() == 1 &&
            doc.frame()->draws.size() == 1 &&
            doc.frame()->render.instances.size() == 1,
        "native cloth/particle are not fake rigid shapes");
  auto frame = doc.frame();
  const auto owner = cloud(doc).key;
  const auto cloth = frame->deformables.cloths.front().key;
  check(frame->deformables.cloths[0].triangle_indices.empty() &&
            frame->deformables.cloths[0].triangle_topology_status ==
                d::DataStatus::not_recorded,
        "no fabricated native cloth faces");
  near(frame->deformables.cloths[0].world_positions[1], {10, 1, 0},
       "cloth pose is applied exactly once");
  check(frame->bounds && frame->bounds->maximum.x >= 10,
        "fit bounds include deformables");
  auto hit = pick_deformable(*frame, {{2, 0, 5}, {0, 0, -2}});
  check(hit && hit->key.kind == d::ElementKind::particle &&
            hit->key.index == 7 && hit->key.object == owner,
        "ray picks sparse SDK index");
  near(static_cast<float>(hit->distance), 5, "normalized pick distance");
  check(doc.select(doc.frame()->draws.front().key), "select rigid shape");
  check(doc.select_deformable(hit->key) && !doc.selected_key() &&
            doc.inspected_object()->id == owner.owner.id,
        "native selection clears shape and inspects real owner");
  check(!frame->deformable_selection,
        "published historical frame remains immutable");
  auto selected = doc.selected_deformable();
  check(selected && doc.frame()->deformable_selection == selected,
        "native highlight published");
  doc.set_frame(1);
  check(!doc.selected_deformable(),
        "element selection cannot cross sample/slot release");
  check(!doc.select_deformable(hit->key), "old sample element key is rejected");
  check(doc.select_deformable({owner, d::ElementKind::owner, 0, 0}),
        "select persistent native owner");
  doc.set_frame(2);
  check(doc.selected_deformable() && cloud(doc).particles.empty() &&
            !cloud(doc).world_bounds,
        "owner survives empty particle transition without stale points");
  check(doc.frame()->deformables.cloths[0].sleeping == true,
        "cloth sleeping state retained");
  doc.set_frame(3);
  check(cloud(doc).particles[0].index == 7 &&
            cloud(doc).particles[0].position->x == 8 &&
            doc.frame()->deformables.cloths[0].sleeping == false,
        "same slot recreated and cloth awake");
  doc.set_frame(4);
  check(!doc.selected_deformable() && cloud(doc).key.owner.generation == 1 &&
            doc.frame()->deformables.cloths[0].key.owner.generation == 1,
        "owner lifetime reuse clears selection");
  check(!doc.select_deformable({owner, d::ElementKind::owner, 0, 0}),
        "stale owner generation cannot select replacement");
  doc.set_frame(0);
  check(doc.inspect(10) && doc.selected_deformable()->object == cloth &&
            !doc.selected_key(),
        "source object inspector selects native owner");
  auto cloth_hit = pick_deformable(*doc.frame(), {{10, 1, 5}, {0, 0, -1}});
  check(cloth_hit && cloth_hit->key.kind == d::ElementKind::cloth_vertex &&
            cloth_hit->key.index == 1,
        "cloth vertex world-space picking");
  doc.select_deformable(cloth_hit->key);
  OrbitCamera camera;
  check(doc.fit_selected(camera, 1.7), "fit native selected element");
  near(camera.target, {10, 1, 0}, "fit selected exact vertex");
  check(doc.fit_deformable(owner, camera) && camera.target.x == 3,
        "fit sparse owner extent");
  doc.set_deformable_visible(owner, false);
  check(!doc.deformable_visible(owner) &&
            !pick_deformable(*doc.frame(), {{2, 0, 5}, {0, 0, -1}}),
        "hidden native cloud not pickable");
  doc.set_frame(1);
  check(!doc.deformable_visible(owner), "owner hide survives seek");
  doc.set_frame(4);
  check(doc.deformable_visible(cloud(doc).key),
        "hide does not cross owner generation");
  doc.set_frame(0);
  check(!doc.deformable_visible(owner),
        "reverse seek restores old lifetime hide");
  doc.show_all();
  doc.isolate_deformable(owner);
  check(doc.isolated() && doc.deformable_visible(owner) &&
            !doc.deformable_visible(cloth) && !doc.frame()->draws[0].visible,
        "native isolate excludes rigid and other native owners");
  doc.show_all();
  doc.isolate(doc.frame()->draws[0].key);
  check(!doc.deformable_visible(owner) && !doc.deformable_visible(cloth),
        "shape isolate excludes native owners");
  doc.show_all();
  doc.set_scene(2);
  check(!doc.selected_deformable() &&
            doc.frame()->deformables.cloths.size() == 1 &&
            doc.frame()->deformables.cloths[0].key.owner.id == 20 &&
            doc.frame()->deformables.particle_clouds.empty(),
        "scene isolation has no foreign native owners");
  check(!pick_deformable(*doc.frame(), {{2, 0, 5}, {0, 0, -1}}),
        "foreign-scene particle cannot be picked");
  doc.set_scene(1);
  DeformablePickOptions opts;
  opts.particles = false;
  check(!pick_deformable(*doc.frame(), {{2, 0, 5}, {0, 0, -1}}, opts),
        "particle view mask honored by pick");
  opts = {};
  opts.angular_radius = .01;
  opts.world_radius = 0;
  check(pick_deformable(*doc.frame(), {{2.03f, 0, 5}, {0, 0, -1}}, opts)
            .has_value(),
        "perspective pixel tolerance");
  check(!pick_deformable(*doc.frame(), {{2, 0, -5}, {0, 0, -1}}),
        "points behind ray ignored");
  doc.clear_selection();
  check(!doc.selected_key() && !doc.selected_deformable() &&
            !doc.inspected_object(),
        "clear both selection domains");
  std::cout << "Native deformable model/picking/lifetimes passed\n";
}
struct Output {
  std::string text;
  float overflow = 0;
};
Output draw(Document &doc, OrbitCamera &camera, RenderOptions &options,
            DeformablePanelState &panel, float width,
            std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
  auto &io = ImGui::GetIO();
  io.DisplaySize = {width, 1100};
  io.DeltaTime = 1.f / 60;
  if (mouse)
    io.AddMousePosEvent(mouse->x, mouse->y);
  if (down)
    io.AddMouseButtonEvent(0, *down);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({width, 1000});
  ImGui::Begin("Native deformables test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
  ImGui::LogToBuffer();
  drawDeformables(doc, camera, options, panel);
  Output out;
  out.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  auto *window = ImGui::GetCurrentWindow();
  out.overflow =
      std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
  ImGui::End();
  ImGui::Render();
  return out;
}
void widgets(float width) {
  Fixture fixture;
  Document doc;
  fixture.load(doc);
  OrbitCamera camera;
  RenderOptions options;
  DeformablePanelState panel;
  doc.inspect(10);
  draw(doc, camera, options, panel, width);
  auto output = draw(doc, camera, options, panel, width);
  check(output.text.find("Surface faces: not recorded") != std::string::npos &&
            output.text.find("Native cloth") != std::string::npos,
        "real ImGui cloth availability text");
  // First actual checkbox controls the real renderer options, not a UI mock.
  const auto &style = ImGui::GetStyle();
  const ImVec2 at{style.WindowPadding.x + 7,
                  style.WindowPadding.y + ImGui::GetFrameHeight() * .5f};
  draw(doc, camera, options, panel, width, at, false);
  draw(doc, camera, options, panel, width, at, true);
  draw(doc, camera, options, panel, width, at, false);
  check(!options.show_cloth,
        "actual Cloth checkbox controls native viewport option");
  doc.inspect(30);
  panel.element_index = 7;
  auto selection = doc.selected_deformable();
  check(selection && selection->kind == d::ElementKind::owner,
        "particle owner inspector");
  output = draw(doc, camera, options, panel, width);
  check(output.text.find("original sparse SDK slots") != std::string::npos &&
            output.text.find("Positions: Captured") != std::string::npos,
        "particle UI shows real channels and identity contract");
  std::snprintf(panel.search, sizeof(panel.search), "Pinned");
  output = draw(doc, camera, options, panel, width);
  check(output.text.find("Sparse cloud") == std::string::npos &&
            output.text.find("Pinned native cloth") != std::string::npos,
        "native owner filter");
  check(output.overflow <= 1, "deformable panel overflows available width");
  std::cout << "Native ImGui panel width=" << width << " passed\n";
}
void real(const char *path) {
  Document doc;
  doc.open_file(path);
  check(doc.timeline().size() == 8, "real SDK deformable frame count");
  check(doc.frame()->draws.empty() && doc.frame()->render.instances.empty(),
        "real SDK native owners are not fake shapes");
  check(doc.frame()->deformables.cloths.size() == 1 &&
            doc.frame()->deformables.particle_clouds.size() == 4,
        "real SDK owners published");
  const auto first = doc.frame();
  const auto &cloth = first->deformables.cloths[0];
  check(cloth.world_positions.size() == 16 && cloth.constraints.size() > 16 &&
            cloth.triangle_indices.empty(),
        "real SDK exact cloth particles/constraints no faces");
  check(std::abs(cloth.world_positions[0].x - cloth.local_positions[0].x) > 1,
        "real cloth world pose applied");
  DeformablePickOptions opts;
  opts.particles = false;
  opts.world_radius = .001;
  const auto point = cloth.world_positions[0];
  auto hit = pick_deformable(
      *first, {{point.x, point.y, point.z + 5}, {0, 0, -1}}, opts);
  check(hit && hit->key.object == cloth.key &&
            hit->key.kind == d::ElementKind::cloth_vertex,
        "real cloth pick");
  check(doc.select_deformable(hit->key), "real SDK element selection");
  doc.set_frame(1);
  check(!doc.selected_deformable(), "real seek clears element selection");
  doc.select_deformable({cloth.key, d::ElementKind::owner, 0, 0});
  doc.set_frame(3);
  check(doc.frame()->deformables.cloths[0].sleeping == true,
        "real SDK sleeping cloth published");
  bool empty = false;
  for (const auto &cloud : doc.frame()->deformables.particle_clouds)
    empty |= cloud.declared_count == 0u && cloud.particles.empty() &&
             !cloud.world_bounds;
  check(empty, "real zero particles do not show stale arrays");
  doc.set_frame(4);
  check(doc.frame()->deformables.cloths[0].sleeping == false, "real SDK wake");
  doc.set_frame(6);
  check(!doc.selected_deformable() &&
            !(doc.frame()->deformables.cloths[0].key == cloth.key),
        "real recreated cloth lifetime resets selection");
  doc.set_frame(0);
  check(doc.frame()->deformables.cloths[0].key == cloth.key,
        "real reverse seek original lifetime");
  OrbitCamera camera;
  RenderOptions options;
  DeformablePanelState panel;
  doc.inspect(cloth.key.owner.id);
  auto output = draw(doc, camera, options, panel, 500);
  check(output.text.find("Surface faces: not recorded") != std::string::npos,
        "real SDK availability visible in real ImGui");
  std::cout << "Real SDK deformable viewer passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    model();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    widgets(1000);
    widgets(420);
    if (argc == 2)
      real(argv[1]);
    else
      check(argc == 1, "usage: pvd_deformable_viewer_tests [capture.pxd2]");
    ImGui::DestroyContext();
    std::cout << "Deformable viewer tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
