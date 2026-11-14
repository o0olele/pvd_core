#include "pvd/coordinates.hpp"
#include "pvd/writer.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
void check(bool v, const char *message) {
  if (!v)
    throw std::runtime_error(message);
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  explicit Fixture(ByteOrder order, std::uint8_t width) {
    header.byte_order = order;
    header.stream_flags = width == 8 ? 1 : 0;
    for (auto *n : {"PxTransform", "PxVec3", "PxVec4", "VoidPtr", "ObjectRef",
                    "PxScene", "PxRigidStatic", "PxRigidDynamic",
                    "PxArticulation", "PxArticulationLink", "PxCloth",
                    "PxParticleSystem", "PxFixedJoint", "Custom"})
      emit(CreateClass{type(n)});
    for (auto *c : {"PxRigidStatic", "PxRigidDynamic", "PxCloth",
                    "PxParticleSystem", "PxArticulation", "Custom"})
      property(c, "Scene", "ObjectRef");
    for (auto *c : {"PxRigidStatic", "PxRigidDynamic", "PxCloth",
                    "PxArticulationLink", "Custom"})
      property(c, "GlobalPose", "PxTransform");
    property("PxArticulationLink", "Parent", "ObjectRef");
    property("PxRigidStatic", "LocalPose", "PxTransform");
    property("PxParticleSystem", "Positions", "PxVec3", true);
    property("PxParticleSystem", "ProjectionPlane", "PxVec4");
    for (auto *p : {"Actors.actor0", "Actors.actor1"})
      property("PxFixedJoint", p, "ObjectRef");
    for (auto *p : {"LocalPose[eACTOR0]", "LocalPose[eACTOR1]"})
      property("PxFixedJoint", p, "PxTransform");
    create("PxScene", 1);
    create("PxScene", 2);
    create("PxRigidStatic", 10);
    create("PxRigidDynamic", 11);
    create("PxArticulation", 20);
    create("PxArticulationLink", 21);
    create("PxCloth", 30);
    create("PxParticleSystem", 40);
    create("PxFixedJoint", 50);
    create("PxRigidStatic", 60);
    create("Custom", 70);
    for (auto object : {10u, 11u, 20u, 30u, 40u, 70u})
      ref(object, "Scene", 1);
    ref(60, "Scene", 2);
    ref(21, "Parent", 20);
    ref(50, "Actors.actor0", 0);
    ref(50, "Actors.actor1", 10);
    for (auto object : {10u, 11u, 21u, 30u, 60u, 70u})
      pose(object, "GlobalPose", 100);
    pose(10, "LocalPose", 7);
    pose(50, "LocalPose[eACTOR0]", 100);
    pose(50, "LocalPose[eACTOR1]", 3);
    set(40, "Positions", "PxVec3", floats({100, 20, 30, 110, 25, 35}), 2);
    set(40, "ProjectionPlane", "PxVec4", floats({.25f, .5f, .75f, 3.f}));
  }
  template <class T> void emit(T v) {
    events.push_back({1, events.size(), events.size(), Payload{std::move(v)}});
  }
  std::uint32_t text(const std::string &s) {
    auto found = handles.find(s);
    if (found != handles.end())
      return found->second;
    auto n = static_cast<std::uint32_t>(handles.size() + 1);
    handles[s] = n;
    emit(StringHandleEvent{s, n});
    return n;
  }
  WireName type(const char *n) {
    auto ns = text("physx3"), name = text(n);
    return {ns, name};
  }
  void create(const char *c, std::uint64_t id) {
    emit(CreateInstance{type(c), id});
  }
  void property(const char *c, const char *p, const char *t, bool arr = false) {
    auto cl = type(c), ty = type(t);
    auto name = text(p);
    emit(CreateProperty{
        cl, name, 0, ty, static_cast<std::uint8_t>(arr ? 2 : 1), {}});
  }
  Bytes integer(std::uint64_t n, std::size_t width) {
    Bytes b;
    for (std::size_t i = 0; i < width; ++i)
      b.push_back(std::uint8_t(
          n >>
          (8 * (header.byte_order == ByteOrder::little ? i : width - 1 - i))));
    return b;
  }
  Bytes floats(std::initializer_list<float> values) {
    Bytes b;
    for (float v : values) {
      std::uint32_t n;
      std::memcpy(&n, &v, 4);
      auto part = integer(n, 4);
      b.insert(b.end(), part.begin(), part.end());
    }
    return b;
  }
  void set(std::uint64_t id, const char *p, const char *t, Bytes b,
           std::uint32_t n = 1) {
    auto name = text(p);
    auto ty = type(t);
    emit(SetPropertyValue{id, name, ty, n, std::move(b)});
  }
  void ref(std::uint64_t id, const char *p, std::uint64_t to) {
    set(id, p, "VoidPtr", integer(to, header.pointer_width()));
  }
  void pose(std::uint64_t id, const char *p, float x) {
    set(id, p, "PxTransform", floats({0, 0, 0, 1, x, 20, 30}));
  }
};
const PropertyValue &prop(const Snapshot &s, std::uint64_t id,
                          const char *p = "GlobalPose") {
  auto *o = s.instance(id);
  check(o, "object");
  auto *v = o->property(p);
  check(v, "property");
  return *v;
}
float x(const Snapshot &s, std::uint64_t id, const char *p = "GlobalPose") {
  return coordinates::native_transform(prop(s, id, p))->position.x;
}
void run(ByteOrder order, std::uint8_t width) {
  Fixture f(order, width);
  const auto before = f.events.size();
  f.emit(OriginShift{1, {10, 2, -3}});
  const auto shifted = f.events.size();
  f.pose(10, "GlobalPose", 100);
  const auto refreshed = f.events.size();
  f.emit(OriginShift{1, {-5, 0, 0}});
  const auto second = f.events.size();
  f.ref(10, "Scene", 2);
  f.emit(OriginShift{1, {20, 0, 0}});
  const auto moved = f.events.size();
  f.emit(DestroyInstance{10});
  f.create("PxRigidStatic", 10);
  f.ref(10, "Scene", 1);
  f.pose(10, "GlobalPose", 7);
  SessionOptions opts;
  opts.checkpoint_interval = 7;
  opts.max_checkpoints = 3;
  Session s(f.header, f.events, opts);
  opts.max_checkpoints = 0;
  Session plain(f.header, f.events, opts);
  for (auto n : {before, shifted, refreshed, second, moved, f.events.size()}) {
    auto a = s.snapshot_at_event(n), b = plain.snapshot_at_event(n);
    check(coordinates::native_bytes(prop(a, 11)) ==
              coordinates::native_bytes(prop(b, 11)),
          "checkpoint coordinate mismatch");
  }
  auto a = s.snapshot_at_event(shifted);
  check(x(a, 10) == 90 && x(a, 11) == 90 && x(a, 21) == 90 && x(a, 30) == 90,
        "world owners not shifted");
  check(x(a, 60) == 100 && x(a, 70) == 100, "other scene/custom class shifted");
  check(x(a, 10, "LocalPose") == 7 && x(a, 50, "LocalPose[eACTOR1]") == 3,
        "local pose shifted");
  check(x(a, 50, "LocalPose[eACTOR0]") == 90, "world joint anchor not shifted");
  check(coordinates::native_vec3(prop(a, 40, "Positions"), 1)->x == 100,
        "particle vector shift");
  check(coordinates::native_bytes(prop(a, 40, "ProjectionPlane")) ==
            f.floats({.25f, .5f, .75f, 4.25f}),
        "particle plane distance shift");
  check(prop(a, 10).as_transform()->position.x == 100, "raw pose was mutated");
  check(x(s.snapshot_at_event(refreshed), 10) == 100,
        "same-byte refresh compacted across epoch");
  check(x(s.snapshot_at_event(second), 10) == 105,
        "second shift after refresh");
  check(x(s.snapshot_at_event(moved), 10) == 105 &&
            x(s.snapshot_at_event(moved), 11) == 75,
        "scene membership changed retroactively");
  check(x(s.latest(), 10) == 7 && s.latest().instance(10)->generation == 1,
        "reuse leaked coordinate epoch");
  check(x(s.snapshot_at_event(before), 10) == 100,
        "reverse query mutated history");
  // A crop rebases bootstrap world bytes after ownership and retained offsets.
  auto crop = writing::crop_events(s, shifted, f.events.size());
  Session reopened(crop.header, crop.events);
  for (auto n : {std::size_t(0), refreshed - shifted, second - shifted,
                 moved - shifted, f.events.size() - shifted}) {
    auto original = s.snapshot_at_event(shifted + n);
    auto restored = reopened.snapshot_at_event(crop.bootstrap_event_count + n);
    for (auto id : {10u, 11u, 21u, 30u, 60u, 70u})
      check(x(original, id) == x(restored, id),
            "crop continuation coordinate mismatch");
    check(coordinates::native_bytes(prop(original, 40, "Positions")) ==
              coordinates::native_bytes(prop(restored, 40, "Positions")),
          "crop particles mismatch");
    check(coordinates::native_bytes(prop(original, 40, "ProjectionPlane")) ==
              coordinates::native_bytes(prop(restored, 40, "ProjectionPlane")),
          "crop particle plane mismatch");
  }
  std::ostringstream out(std::ios::binary);
  writing::write_session(out, s);
  const auto bytes = out.str();
  auto saved = Session::parse(Bytes(bytes.begin(), bytes.end()));
  check(prop(saved.latest(), 11).raw_bytes() ==
            prop(s.latest(), 11).raw_bytes(),
        "full save changed raw bytes");
  check(x(saved.latest(), 11) == x(s.latest(), 11),
        "full save coordinates mismatch");
}
void rounding() {
  Fixture f(ByteOrder::little, 8);
  f.pose(10, "GlobalPose", 1000000.f);
  float oracle = 1000000.f;
  for (unsigned i = 0; i < 1000; ++i) {
    f.emit(OriginShift{1, {.01f, 0, 0}});
    oracle -= .01f;
  }
  Session s(f.header, f.events);
  auto a = s.latest();
  check(x(a, 10) == oracle && oracle == 1000000.f,
        "sequential SDK float rounding lost");
  check(prop(a, 10).as_transform()->position.x == 1000000.f,
        "rounding changed wire source");
  check(a.origin_shifts.at(1).x > 9.9f,
        "rounding fixture did not accumulate shifts");
  // Within a coordinate epoch, identical writes retain the old immutable bytes.
  Fixture g(ByteOrder::little, 8);
  Session live(g.header);
  live.append(g.events);
  auto bytes = prop(live.latest(), 10).bytes;
  auto n = g.events.size();
  for (unsigned i = 0; i < 100; ++i)
    g.pose(10, "GlobalPose", 100);
  live.append(std::vector<Event>(g.events.begin() + n, g.events.end()));
  check(prop(live.latest(), 10).bytes == bytes,
        "same-epoch no-op compaction regressed");
  auto prefix = live.event_count();
  try {
    live.append({Event{
        1, 0, 0,
        OriginShift{1, {std::numeric_limits<float>::quiet_NaN(), 0, 0}}}});
    check(false, "nonfinite shift accepted");
  } catch (const std::invalid_argument &) {
  }
  check(live.failed() && live.event_count() == prefix &&
            x(live.latest(), 10) == 100,
        "rejected shift damaged successful prefix");
}
} // namespace
int main() {
  try {
    for (auto order : {ByteOrder::little, ByteOrder::big})
      for (auto width : {std::uint8_t(4), std::uint8_t(8)})
        run(order, width);
    rounding();
    std::cout << "coordinates: raw/native epochs, lifecycle, scene migration, "
                 "replay, crop and float rounding passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
