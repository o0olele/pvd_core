#include <cstring>
#include <iostream>
#include <limits>
#include <pvd/debug_render.hpp>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::render;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Writer {
  ByteOrder order;
  Bytes bytes;
  explicit Writer(ByteOrder o = ByteOrder::little) : order(o) {}
  void u(std::uint64_t v, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          v >> ((order == ByteOrder::little ? i : width - i - 1) * 8)));
  }
  void f(float v) {
    std::uint32_t b;
    std::memcpy(&b, &v, 4);
    u(b, 4);
  }
  void vec(float x = 1, float y = 2, float z = 3) {
    f(x);
    f(y);
    f(z);
  }
  void transform() {
    f(0);
    f(0);
    f(0);
    f(1);
    vec();
  }
  void point(std::uint32_t c = 0x12345678) {
    vec();
    u(c, 4);
  }
  void line() {
    point(0xff112233);
    point(0xff445566);
  }
  void tri() {
    point(0xff010203);
    point(0xff040506);
    point(0xff070809);
  }
  void text(const std::string &s = "hi") {
    u(0xaabbccdd, 4);
    vec();
    f(1.5f);
    u(s.size() + 1, 4);
    for (char c : s)
      u(static_cast<std::uint8_t>(c), 1);
    u(0, 1);
  }
};
PropertyValue value(Bytes b, ByteOrder order = ByteOrder::little,
                    std::uint8_t width = 8, std::uint64_t count = 1,
                    QualifiedName type = {"physx3", "PvdU8"}) {
  PropertyValue v;
  v.bytes = std::make_shared<const Bytes>(std::move(b));
  v.byte_order = order;
  v.pointer_width = width;
  v.item_count = count;
  v.incoming_type = std::move(type);
  return v;
}
ChannelPacket packet(const Writer &w, std::size_t event,
                     std::uint64_t stream = 7, std::uint64_t renderer = 100) {
  return {event,
          stream,
          renderer,
          "events",
          "render event stream",
          value(w.bytes, w.order)};
}
Metadata metadata() {
  Metadata m;
  m.classes.emplace(QualifiedName{"physx3", "PxScene"},
                    ClassDefinition{{"physx3", "PxScene"}, {}, {}});
  m.classes.emplace(QualifiedName{"physx3", "PxRigidDynamic"},
                    ClassDefinition{{"physx3", "PxRigidDynamic"}, {}, {}});
  return m;
}
Snapshot snapshot() {
  Snapshot s;
  for (auto id : {1ull, 2ull}) {
    InstanceState scene;
    scene.id = id;
    scene.class_name = {"physx3", "PxScene"};
    s.instances.emplace(id, scene);
  }
  for (auto id : {10ull, 20ull}) {
    InstanceState actor;
    actor.id = id;
    actor.class_name = {"physx3", "PxRigidDynamic"};
    auto v = value({});
    v.reference_ids = std::vector<std::uint64_t>{id == 10 ? 1ull : 2ull};
    actor.properties.emplace("Scene", v);
    s.instances.emplace(id, actor);
  }
  return s;
}
Writer all(ByteOrder order) {
  Writer w(order);
  w.u(1, 1);
  w.u(10, 8);
  w.u(2, 1);
  w.u(1, 4);
  w.point();
  w.u(3, 1);
  w.u(1, 4);
  w.line();
  w.u(4, 1);
  w.u(1, 4);
  w.tri();
  w.u(5, 1);
  w.transform();
  w.transform();
  w.u(6, 1);
  w.transform();
  w.transform();
  w.f(7.25f);
  w.u(1, 1);
  w.u(7, 1);
  w.transform();
  w.f(-.5f);
  w.f(.75f);
  w.u(0, 1);
  w.u(8, 1);
  w.transform();
  w.f(.2f);
  w.f(.3f);
  w.u(1, 1);
  w.u(9, 1);
  w.transform();
  w.f(.4f);
  w.u(0, 1);
  w.u(10, 1);
  w.text();
  w.u(11, 1);
  w.u(1, 4);
  w.point();
  w.u(1, 4);
  w.line();
  w.u(1, 4);
  w.tri();
  return w;
}
void debug_formats() {
  for (auto order : {ByteOrder::little, ByteOrder::big})
    for (auto width : {std::uint8_t{4}, std::uint8_t{8}}) {
      auto w = all(order);
      auto p = packet(w, 10);
      p.value.pointer_width = width;
      auto result = extract_debug(snapshot(), metadata(), {p}, {10, 11, 1, {}});
      check(result.complete, "all debug tags decode");
      check(result.points.size() == 2 && result.lines.size() == 2 &&
                result.triangles.size() == 2 && result.texts.size() == 1 &&
                result.constraints.size() == 5,
            "all debug tag counts");
      check(result.points[0].source.instance_id == 10 &&
                result.points[0].color == 0x12345678 &&
                result.points[0].position.y == 2,
            "point position/color/id");
      check(result.lines[0].color1 == 0xff445566 &&
                result.triangles[0].color2 == 0xff070809,
            "per vertex colors");
      check(result.texts[0].color == 0xaabbccdd &&
                result.texts[0].size == 1.5f && result.texts[0].text == "hi",
            "text color position size string");
      check(
          std::get<DebugLinearLimit>(result.constraints[1].payload).value ==
                  7.25f &&
              std::get<DebugLinearLimit>(result.constraints[1].payload).active,
          "linear limit exact data");
      check(
          std::get<DebugAngularLimit>(result.constraints[2].payload).lower ==
                  -.5f &&
              std::get<DebugLimitCone>(result.constraints[3].payload).z_swing ==
                  .3f,
          "angular/cone exact data");
      DebugOptions opts;
      opts.joint_axis_length = .5f;
      auto axes =
          extract_debug(snapshot(), metadata(), {p}, {10, 11, 1, {}}, opts);
      check(axes.lines.size() == 8 && axes.lines[2].position1.y == 2.5f,
            "joint transforms generate optional scaled axes");
    }
}
void ranges_and_state() {
  Writer id;
  id.u(1, 1);
  id.u(10, 8);
  Writer a;
  a.u(2, 1);
  a.u(1, 4);
  a.point(0xff000001);
  Writer b;
  b.u(2, 1);
  b.u(1, 4);
  b.point(0xff000002);
  std::vector<ChannelPacket> packets = {packet(id, 1), packet(a, 5),
                                        packet(b, 15)};
  auto first = extract_debug(snapshot(), metadata(), packets, {5, 6, 1, {}});
  auto next = extract_debug(snapshot(), metadata(), packets, {15, 16, 1, {}});
  check(first.points.size() == 1 && next.points.size() == 1 &&
            next.points[0].color == 0xff000002 &&
            next.points[0].source.instance_id == 10,
        "new frame clears primitives while preserving earlier SetInstanceId");
  Writer historical;
  historical.u(1, 1);
  historical.u(10, 8);
  historical.u(2, 1);
  historical.u(2, 4);
  historical.point();
  historical.point();
  DebugOptions one_primitive;
  one_primitive.max_primitives = 1;
  auto budget = extract_debug(snapshot(), metadata(),
                              {packet(historical, 1), packet(a, 5)},
                              {5, 6, 1, {}}, one_primitive);
  check(budget.complete && budget.points.size() == 1,
        "historical primitives neither consume current frame budget nor "
        "allocate output");
  check(extract_debug(snapshot(), metadata(), packets, {6, 15, 1, {}})
            .points.empty(),
        "range is half open");
  Writer foreign;
  foreign.u(1, 1);
  foreign.u(20, 8);
  foreign.u(2, 1);
  foreign.u(1, 4);
  foreign.point();
  packets.insert(packets.begin() + 2, packet(foreign, 5, 8, 101));
  auto scene1 = extract_debug(snapshot(), metadata(), packets, {5, 6, 1, {}});
  auto scene2 = extract_debug(snapshot(), metadata(), packets, {5, 6, 2, {}});
  check(scene1.points.size() == 1 &&
            scene1.points[0].source.instance_id == 10 &&
            scene2.points.size() == 1 &&
            scene2.points[0].source.instance_id == 20,
        "scene/stream isolation");
  Writer empty_buffer;
  empty_buffer.u(11, 1);
  empty_buffer.u(0, 4);
  empty_buffer.u(0, 4);
  empty_buffer.u(0, 4);
  auto empty_result = extract_debug(snapshot(), metadata(),
                                    {packet(empty_buffer, 5)}, {5, 6, 1, {}});
  check(empty_result.complete && empty_result.points.empty() &&
            empty_result.diagnostics.empty(),
        "empty SDK debug buffers require no invented scene association");
  auto unknown =
      extract_debug(snapshot(), metadata(), {packet(a, 5)}, {5, 6, 1, {}});
  check(unknown.points.empty() && !unknown.complete &&
            unknown.diagnostics.back().code == DebugCode::unattributed_scene,
        "unattributed channel cannot leak into scene");
  auto explicit_scope =
      extract_debug(snapshot(), metadata(), {packet(a, 5)}, {5, 6, 1, {100}});
  check(explicit_scope.points.size() == 1 &&
            explicit_scope.points[0].source.instance_id == 0,
        "explicit renderer ownership permits unlabeled renderbuffer");
  auto invalid = extract_debug(snapshot(), metadata(), packets, {6, 5, 1, {}});
  check(!invalid.complete &&
            invalid.diagnostics[0].code == DebugCode::invalid_range,
        "invalid interval diagnosed");
}
void malformed() {
  Writer w;
  w.u(1, 1);
  w.u(10, 8);
  w.u(2, 1);
  w.u(1, 4);
  w.point();
  w.u(4, 1);
  w.u(1, 4);
  w.vec();
  auto result =
      extract_debug(snapshot(), metadata(), {packet(w, 1)}, {1, 2, 1, {}});
  check(!result.complete && result.points.size() == 1 &&
            result.triangles.empty(),
        "malformed tail cannot partially publish primitive");
  Writer unknown;
  unknown.u(1, 1);
  unknown.u(10, 8);
  unknown.u(99, 1);
  unknown.u(2, 1);
  unknown.u(1, 4);
  unknown.point();
  result = extract_debug(snapshot(), metadata(), {packet(unknown, 1)},
                         {1, 2, 1, {}});
  check(result.points.empty() && !result.complete &&
            result.diagnostics.back().code == DebugCode::unsupported_event,
        "unknown tags must stop rather than misalign");
  Writer count;
  count.u(2, 1);
  count.u(0xffffffff, 4);
  result =
      extract_debug(snapshot(), metadata(), {packet(count, 1)}, {1, 2, {}, {}});
  check(!result.complete &&
            result.diagnostics.back().code == DebugCode::limit_exceeded,
        "malicious bulk count bounded before allocation");
  Writer text;
  text.u(10, 1);
  text.text();
  text.bytes.back() = 'x';
  result =
      extract_debug(snapshot(), metadata(), {packet(text, 1)}, {1, 2, {}, {}});
  check(result.texts.empty() && !result.complete,
        "malformed text terminator diagnosed");
  auto full = all(ByteOrder::little);
  DebugOptions limit;
  limit.max_primitives = 1;
  result = extract_debug(snapshot(), metadata(), {packet(full, 1)},
                         {1, 2, 1, {}}, limit);
  check(result.points.size() == 1 && result.lines.empty() && !result.complete,
        "cumulative primitive limit");
  limit = {};
  limit.max_text_bytes = 1;
  result = extract_debug(snapshot(), metadata(), {packet(full, 1)},
                         {1, 2, 1, {}}, limit);
  check(result.texts.empty() && !result.complete, "text length limit");
  limit = {};
  limit.max_packet_bytes = 1;
  result = extract_debug(snapshot(), metadata(), {packet(full, 1)},
                         {1, 2, 1, {}}, limit);
  check(result.points.empty() && !result.complete, "packet byte limit");
}
void flattened_joint_scene_ownership() {
  auto snap = snapshot();
  auto meta = metadata();
  meta.classes.emplace(QualifiedName{"physx3", "PxJoint"},
                       ClassDefinition{{"physx3", "PxJoint"}, {}, {}});
  std::vector<ChannelPacket> packets;
  for (auto actor : {10ull, 20ull}) {
    InstanceState joint;
    joint.id = actor + 100;
    joint.class_name = {"physx3", "PxJoint"};
    auto world = value({}); world.reference_ids = std::vector<std::uint64_t>{0};
    auto body = value({}); body.reference_ids = std::vector<std::uint64_t>{actor};
    joint.properties["Actors.actor0"] = world;
    joint.properties["Actors.actor1"] = body;
    snap.instances.emplace(joint.id, joint);
    Writer w;w.u(1, 1);w.u(joint.id, 8);w.u(5, 1);w.transform();w.transform();
    packets.push_back(packet(w, packets.size() + 3));
  }
  const auto first = extract_debug(snap, meta, packets, {3, 5, 1, {100}});
  const auto second = extract_debug(snap, meta, packets, {3, 5, 2, {100}});
  check(first.constraints.size() == 1 && second.constraints.size() == 1 &&
            first.constraints[0].source.instance_id == 110 &&
            second.constraints[0].source.instance_id == 120,
        "native flattened joint references lost scene ownership or leaked across scenes");
}
Writer contact(ByteOrder order, std::uint8_t width, bool force = true) {
  Writer w(order);
  w.vec(1, 2, 3);
  w.vec(0, 1, 0);
  w.u(width == 8 ? 0x1122334455667788ull : 0x55667788ull, width);
  w.u(width == 8 ? 0x8877665544332211ull : 0x44332211ull, width);
  w.f(-.125f);
  w.f(force ? 42.5f : std::numeric_limits<float>::quiet_NaN());
  w.u(0xffffffffu, 4);
  w.u(17, 4);
  w.u(force ? 1 : 0, 1);
  while (w.bytes.size() % (width == 8 ? 64 : 52))
    w.u(0xcc, 1);
  return w;
}
void contacts() {
  for (auto order : {ByteOrder::little, ByteOrder::big})
    for (auto width : {std::uint8_t{4}, std::uint8_t{8}}) {
      auto snap = snapshot();
      auto w = contact(order, width);
      snap.instances.at(1).properties["Contacts"] =
          value(w.bytes, order, width, 1, {"physx3", "PvdContact"});
      auto result = extract_contacts(snap, metadata(), 1);
      check(result.complete && result.contacts_available &&
                result.contacts.size() == 1,
            "source ABI contact decoded");
      const auto &c = result.contacts[0];
      check(c.position.z == 3 && c.normal.y == 1 && c.separation == -.125f &&
                c.normal_force == std::optional<float>(42.5f) &&
                c.internal_face_index0 == 0xffffffffu &&
                c.internal_face_index1 == 17,
            "contact fields offsets");
      check(c.shape0 == (width == 8 ? 0x1122334455667788ull : 0x55667788ull),
            "contact source pointer width");
      auto unavailable = contact(order, width, false);
      snap.instances.at(1).properties["Contacts"] =
          value(unavailable.bytes, order, width, 1, {"physx3", "PvdContact"});
      result = extract_contacts(snap, metadata(), 1);
      check(result.complete && !result.contacts[0].normal_force,
            "unavailable normal force never fabricated or read as NaN");
      w.bytes.pop_back();
      snap.instances.at(1).properties["Contacts"] =
          value(w.bytes, order, width, 1, {"physx3", "PvdContact"});
      result = extract_contacts(snap, metadata(), 1);
      check(!result.complete && result.contacts.empty(),
            "malformed contact tail rejected");
    }
  auto snap = snapshot();
  snap.instances.at(1).properties["Contacts"] =
      value({}, ByteOrder::little, 8, 0, {"physx3", "PvdContact"});
  auto result = extract_contacts(snap, metadata(), 1);
  check(result.complete && result.contacts_available && result.contacts.empty(),
        "collected empty contact array");
  snap.instances.at(1).properties["Contacts"].incoming_type = {"custom",
                                                               "PvdContact"};
  result = extract_contacts(snap, metadata(), 1);
  check(!result.complete &&
            result.diagnostics[0].code == DebugCode::unsupported_contacts,
        "unverified contact schema is not reinterpreted");
  snap.instances.at(1).properties["Contacts"] =
      value(contact(ByteOrder::little, 8).bytes, ByteOrder::little, 8, 1,
            {"physx3", "PvdContact"});
  DebugOptions options;
  options.max_contacts = 0;
  result = extract_contacts(snap, metadata(), 1, options);
  check(!result.complete && result.contacts.empty(),
        "contact allocation limit");
}
} // namespace
int main() {
  try {
    debug_formats();
    ranges_and_state();
    malformed();
    flattened_joint_scene_ownership();
    contacts();
    std::cout << "debug render tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "debug render tests: " << e.what() << '\n';
    return 1;
  }
}
