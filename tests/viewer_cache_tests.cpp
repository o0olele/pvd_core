#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  void emit(Payload payload) { events.push_back({1, 0, events.size(), std::move(payload)}); }
  std::uint32_t text(const std::string &value) {
    auto found = strings.find(value);
    if (found != strings.end()) return found->second;
    auto id = static_cast<std::uint32_t>(strings.size() + 1);
    strings[value] = id; emit(StringHandleEvent{value, id}); return id;
  }
  WireName type(const char *value) { auto ns = text("physx3"); return {ns, text(value)}; }
  void property(const char *cls, const char *name, const char *dt, bool array = false) {
    auto c = type(cls), t = type(dt); auto n = text(name);
    emit(CreateProperty{c, n, 0, t, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void value(std::uint64_t id, const char *name, const char *dt, Bytes data, std::uint32_t n = 1) {
    auto p = text(name); auto t = type(dt); emit(SetPropertyValue{id, p, t, n, std::move(data)});
  }
  void name(std::uint64_t id, const char *value) {
    Bytes data(8, 0); data.insert(data.end(), value, value + std::strlen(value) + 1);
    this->value(id, "Name", "String", std::move(data));
  }
  void actor(const char *cls, std::uint64_t id, std::uint64_t scene, const char *label) {
    emit(CreateInstance{type(cls), id});
    Bytes ref; for (unsigned i = 0; i < 8; ++i) ref.push_back(static_cast<std::uint8_t>(scene >> (8 * i)));
    value(id, "Scene", "VoidPtr", std::move(ref));
    value(id, "Shapes", "VoidPtr", {}, 0);
    Bytes pose(28, 0); pose[14] = 0x80; pose[15] = 0x3f;
    value(id, "GlobalPose", "PxTransform", std::move(pose)); name(id, label);
  }
  void frame(std::uint64_t scene) { auto n = text("frame"); emit(BeginSection{scene, n, 1}); emit(EndSection{scene, n, 2}); }
  Fixture() {
    header.stream_flags = 1;
    for (const auto *c : {"PxScene", "PxRigidStatic", "DeferredActor"}) emit(CreateClass{type(c)});
    for (const auto *c : {"PxRigidStatic", "DeferredActor"}) {
      property(c, "Scene", "ObjectRef"); property(c, "Shapes", "ObjectRef", true);
      property(c, "GlobalPose", "PxTransform"); property(c, "Name", "StringHandle");
    }
    emit(CreateInstance{type("PxScene"), 1}); emit(CreateInstance{type("PxScene"), 2});
    actor("PxRigidStatic", 10, 1, "first name"); actor("PxRigidStatic", 20, 2, "other scene");
    actor("DeferredActor", 900, 1, "deferred name"); frame(1); frame(2);
    name(10, "updated name"); frame(1); emit(DestroyInstance{10});
    actor("PxRigidStatic", 10, 1, "new generation"); frame(1);
  }
  std::unique_ptr<Session> session() const { return std::make_unique<Session>(header, events); }
};
ActorNode find(const std::vector<ActorNode> &nodes, std::uint64_t id) {
  auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto &n) { return n.key.id == id; });
  check(found != nodes.end(), "expected actor is absent"); return *found;
}
void noop() {
  Fixture fixture; Document doc; doc.adopt(fixture.session());
  auto before = doc.frame(); doc.set_view_options(doc.view_options());
  check(doc.frame() == before, "same display preferences must preserve immutable FrameState identity");
  auto view = doc.view_options(); view.joint_frame_scale = -0.f;
  doc.set_view_options(view); before = doc.frame();
  check(std::signbit(doc.view_options().joint_frame_scale), "signed zero display size retained");
  doc.set_view_options(view); check(doc.frame() == before, "same signed-zero state is a no-op");
  view.joint_frame_scale = +0.f; doc.set_view_options(view);
  check(doc.frame() != before && !std::signbit(doc.view_options().joint_frame_scale), "different IEEE size bits must not be mistaken for no-op");
  for (auto role : color_palette_roles) {
    view = doc.view_options(); palette_color(view.palette, role)[0] = -0.f; doc.set_view_options(view);
    before = doc.frame(); doc.set_view_options(view); check(before == doc.frame(), "identical palette must not rebuild");
    palette_color(view.palette, role)[0] = +0.f; doc.set_view_options(view);
    check(before != doc.frame() && !std::signbit(palette_color(doc.view_options().palette, role)[0]), "palette signed-zero bits survive setters");
  }
  before = doc.frame(); view = doc.view_options(); view.geometry_scale = 0;
  bool rejected = false; try { doc.set_view_options(view); } catch (const std::invalid_argument &) { rejected = true; }
  check(rejected && before == doc.frame(), "validation precedes no-op and retains prior source/frame");
}
void invalidation() {
  Fixture fixture; Document doc; doc.adopt(fixture.session());
  auto first = doc.actor_tree(); check(find(first, 10).name == "first name", "first actor name");
  check(doc.set_frame(1) && find(doc.actor_tree(), 10).name == "updated name", "cached name follows new prefix");
  check(doc.set_frame(2) && find(doc.actor_tree(), 10).key.generation == 1, "cached actor generation follows ID reuse");
  check(doc.set_frame(0) && find(doc.actor_tree(), 10).key.generation == 0 && find(doc.actor_tree(), 10).name == "first name", "reverse seek restores correct cached lifetime/name");
  check(find(first, 10).name == "first name", "caller-owned tree remains immutable across cache refreshes");
  auto no_frames = fixture.events;
  no_frames.erase(std::remove_if(no_frames.begin(), no_frames.end(), [](const auto &event) {
    return std::holds_alternative<BeginSection>(event.payload) || std::holds_alternative<EndSection>(event.payload);
  }), no_frames.end());
  doc.adopt(std::make_unique<Session>(fixture.header, no_frames)); const auto prefix = doc.frame()->snapshot.event_count;
  check(doc.actor_tree().size() == 1 && find(doc.actor_tree(), 10).key.generation == 1, "no-frame EOF actor tree");
  check(doc.set_scene(2) && doc.frame()->snapshot.event_count == prefix && doc.actor_tree().size() == 1 && find(doc.actor_tree(), 20).key.id == 20, "same-prefix scene switch must invalidate actor cache");
  Document live; live.begin_live(fixture.header);
  Document initial; initial.adopt(fixture.session()); const auto seed = initial.frame()->snapshot.event_count;
  live.append_live(std::vector<Event>(fixture.events.begin(), fixture.events.begin() + seed)); live.set_follow_live(false);
  const auto paused = live.frame(); check(live.actor_tree().size() == 1, "deferred class is initially not a rigid actor");
  const auto parent = fixture.type("PxRigidStatic"), child = fixture.type("DeferredActor");
  live.append_live({Event{1, 0, 0, DeriveClass{parent, child}}});
  check(live.frame() == paused && live.actor_tree().size() == 2 && find(live.actor_tree(), 900).name == "deferred name", "future metadata must invalidate paused-prefix classification");
  const auto start = fixture.events.size(); fixture.name(10, "live new name"); fixture.frame(1);
  live.append_live(std::vector<Event>(fixture.events.begin() + start, fixture.events.end()));
  check(find(live.actor_tree(), 10).name == "first name", "paused cached names cannot adopt future property values");
  live.set_follow_live(true); check(find(live.actor_tree(), 10).name == "live new name", "resume follows current prefix names");
  for (auto &event : fixture.events)
    if (auto *set = std::get_if<SetPropertyValue>(&event.payload); set && set->instance_id == 10 && set->property_name == fixture.strings.at("Name")) {
      set->data.assign(8, 0); const char replacement[] = "replacement source";
      set->data.insert(set->data.end(), replacement, replacement + sizeof(replacement));
    }
  doc.adopt(fixture.session()); check(find(doc.actor_tree(), 10).name == "replacement source", "new Session must never reuse an old source's tree cache");
}
} // namespace
int main() {
  try { noop(); invalidation(); std::cout << "Viewer cache: exact no-op/IEEE values, prefix/name/lifetime/scene/metadata/source invalidation passed\n"; return 0; }
  catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
