#include "pvd/body_overlays.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
namespace {
using namespace pvd;
using namespace pvd::render;
void check(bool ok, const char *text) {
  if (!ok)
    throw std::runtime_error(text);
}
void close(float a, float b, const char *text) {
  check(std::isfinite(a) && std::abs(a - b) < 3e-5f, text);
}
void close(Vec3 a, Vec3 b, const char *text) {
  close(a.x, b.x, text);
  close(a.y, b.y, text);
  close(a.z, b.z, text);
}
bool has(const BodyOverlays &o, BodyOverlayCode c) {
  for (const auto &d : o.diagnostics)
    if (d.code == c)
      return true;
  return false;
}
struct Fixture {
  Snapshot snapshot;
  Frame frame;
  ByteOrder order;
  std::uint8_t width;
  const std::uint64_t a = 0xfedcba9876543210ull, b = 0x8000000000000020ull;
  Fixture(ByteOrder o = ByteOrder::little, std::uint8_t w = 8)
      : order(o), width(w) {
    snapshot.event_count = frame.event_count = 123;
    object(1, 4, "PxScene");
    object(a, 5, "PxRigidDynamic");
    object(b, 8, "PxRigidDynamic");
    object(100, 6, "PxShape");
    object(101, 7, "PxShape");
    reference(a, "Scene", 1);
    reference(b, "Scene", 1);
    pose(a, {10, 20, 30}, true);
    pose(b, {-10, 3, 4});
    vector(a, "LinearVelocity", {5, 0, 0});
    vector(a, "AngularVelocity", {0, -2, 0});
    vector(b, "LinearVelocity", {-3, 4, 0});
    vector(b, "AngularVelocity", {});
    attachment(a, 5, 100, 6, {{1, 2, 3}, {4, 6, 8}});
    attachment(a, 5, 101, 7, {{7, 8, 9}, {10, 12, 14}});
    attachment(b, 8, 100, 6, {{-5, -4, -3}, {-2, 0, 2}});
  }
  void object(std::uint64_t id, std::uint64_t generation, const char *name) {
    InstanceState value;
    value.id = id;
    value.generation = generation;
    value.class_name = {"physx3", name};
    snapshot.instances.emplace(id, std::move(value));
  }
  void append(Bytes &data, std::uint64_t value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i)
      data.push_back(static_cast<std::uint8_t>(
          value >> (8 * (order == ByteOrder::little ? i : bytes - i - 1))));
  }
  void put(std::uint64_t id, const char *name, const char *type, Bytes data,
           std::uint64_t count = 1) {
    PropertyValue p;
    p.incoming_type = {"physx3", type};
    p.item_count = count;
    p.byte_order = order;
    p.pointer_width = width;
    p.bytes = std::make_shared<const Bytes>(std::move(data));
    snapshot.instances.at(id).properties[name] = std::move(p);
  }
  void floats(std::uint64_t id, const char *name, const char *type,
              std::initializer_list<float> values) {
    Bytes raw;
    for (float value : values) {
      std::uint32_t bits;
      std::memcpy(&bits, &value, 4);
      append(raw, bits, 4);
    }
    put(id, name, type, std::move(raw));
  }
  void vector(std::uint64_t id, const char *name, Vec3 v) {
    floats(id, name, "PxVec3", {v.x, v.y, v.z});
  }
  void pose(std::uint64_t id, Vec3 p, bool rotated = false) {
    const float half = rotated ? std::sqrt(.5f) : 0;
    floats(id, "GlobalPose", "PxTransform",
           {0, 0, half, rotated ? half : 1, p.x, p.y, p.z});
  }
  void reference(std::uint64_t id, const char *name, std::uint64_t target) {
    Bytes raw;
    append(raw, target, width);
    put(id, name, "VoidPtr", raw);
    snapshot.instances.at(id).properties.at(name).reference_ids =
        std::vector<std::uint64_t>{target};
  }
  void attachment(std::uint64_t actor, std::uint64_t actor_generation,
                  std::uint64_t shape, std::uint64_t shape_generation,
                  Bounds bounds, ActorKind kind = ActorKind::rigid_dynamic) {
    ShapeInstance value;
    value.actor_id = actor;
    value.actor_generation = actor_generation;
    value.shape_id = shape;
    value.shape_generation = shape_generation;
    value.scene_id = 1;
    value.scene_generation = 4;
    value.geometry = Box{{1, 2, 3}};
    value.actor.kind = kind;
    value.world_pose.position = {500, 600, 700};
    value.world_bounds = bounds;
    frame.instances.push_back(value);
  }
  BodyOverlayOptions all() const {
    BodyOverlayOptions o;
    o.show_bounds = o.show_linear_velocity = o.show_angular_velocity = true;
    return o;
  }
};
void basic(ByteOrder order, std::uint8_t width) {
  Fixture f(order, width);
  const auto before = f.snapshot.instances.at(f.a).properties;
  const auto result = build_body_overlays(f.frame, f.snapshot, f.all());
  check(
      result.complete && result.diagnostics.empty() &&
          result.bounds_count == 3 && result.bounds.size() == 36 &&
          result.linear_vectors == 2 && result.linear_velocity.size() == 6 &&
          result.angular_vectors == 1 && result.angular_velocity.size() == 3,
      "AABB per attachment and one vector per visible scene/actor generation");
  for (const auto *lines :
       {&result.bounds, &result.linear_velocity, &result.angular_velocity})
    for (const auto &line : *lines)
      check(line.derived && line.source.snapshot_event_count == 123 &&
                line.source.key.scene.id == 1 &&
                line.source.key.scene.generation == 4,
            "true picking key, generation, prefix and derived provenance");
  const auto &linear = result.linear_velocity[0];
  check(linear.source.key == f.frame.instances[0].picking_key() &&
            linear.kind == BodyOverlayKind::linear_velocity &&
            linear.part == BodyOverlayPart::vector_shaft,
        "vector representative is a real visible shape attachment");
  close(linear.position0, {10, 20, 30},
        "vector anchor is captured actor GlobalPose, not shape/bounds center");
  close(linear.position1, {11, 20, 30},
        "world velocity is not rotated by actor quaternion");
  close(result.angular_velocity[0].position1, {10, 19.6f, 30},
        "angular velocity axis/magnitude display");
  check(result.linear_velocity[1].part == BodyOverlayPart::arrow_head &&
            result.linear_velocity[2].part == BodyOverlayPart::arrow_head,
        "one complete arrow has two derived head lines");
  check(result.linear_velocity[0].color != result.angular_velocity[0].color &&
            result.bounds[0].color != linear.color,
        "layer colors are distinct");
  check(result.linear_velocity[3].source.key.actor.id == f.b &&
            result.linear_velocity[3].source.key.shape.id == 100,
        "shared shape does not collapse different actors");
  std::set<std::pair<unsigned, unsigned>> edges;
  for (std::size_t i = 0; i < 12; ++i) {
    const auto &line = result.bounds[i];
    auto corner = [&](Vec3 p) {
      check((p.x == 1 || p.x == 4) && (p.y == 2 || p.y == 6) &&
                (p.z == 3 || p.z == 8),
            "tight bounds have no broadphase inflation");
      return unsigned(p.x == 4) | (unsigned(p.y == 6) << 1) |
             (unsigned(p.z == 8) << 2);
    };
    auto a = corner(line.position0), b = corner(line.position1);
    check(a != b && ((a ^ b) == 1 || (a ^ b) == 2 || (a ^ b) == 4),
          "AABB edges connect exactly one axis");
    if (a > b)
      std::swap(a, b);
    edges.emplace(a, b);
  }
  check(edges.size() == 12, "12 unique box edges");
  for (const auto &entry : before)
    check(f.snapshot.instances.at(f.a).properties.at(entry.first).bytes ==
              entry.second.bytes,
          "source property bytes are unchanged");
  check(f.frame.instances[0].world_pose.position.x == 500 &&
            f.frame.instances[0].world_bounds->minimum.x == 1,
        "source render frame unchanged");
}
void visibility() {
  Fixture f;
  auto options = f.all();
  options.visible_attachments =
      std::vector<PickingKey>{f.frame.instances[1].picking_key()};
  f.snapshot.instances.at(f.b).properties.erase("GlobalPose");
  auto result = build_body_overlays(f.frame, f.snapshot, options);
  check(result.complete && result.bounds_count == 1 &&
            result.linear_vectors == 1 && result.angular_vectors == 1 &&
            result.linear_velocity[0].source.key ==
                f.frame.instances[1].picking_key(),
        "hidden first attachment never becomes vector owner or emits "
        "diagnostics");
  options.visible_attachments = std::vector<PickingKey>{};
  f.snapshot.event_count = 999;
  result = build_body_overlays(f.frame, f.snapshot, options);
  check(
      result.complete && result.bounds.empty() &&
          result.linear_velocity.empty() && result.processed_attachments == 0,
      "present empty visibility mask means NONE even with hidden invalid data");
  f.snapshot.event_count = 123;
  options.visible_attachments.reset();
  result = build_body_overlays(f.frame, f.snapshot, options);
  check(!result.complete && has(result, BodyOverlayCode::missing_property),
        "null visibility means all attachments");
}
void static_links_zero_scale() {
  Fixture f;
  f.frame.instances.clear();
  f.object(30, 0, "PxRigidStatic");
  f.object(102, 0, "PxShape");
  f.attachment(30, 0, 102, 0, {{0, 0, 0}, {1, 1, 1}}, ActorKind::rigid_static);
  auto result = build_body_overlays(f.frame, f.snapshot, f.all());
  check(result.complete && result.bounds_count == 1 &&
            result.linear_vectors == 0 && result.angular_vectors == 0,
        "static bodies never guess or require velocities");
  f.object(40, 1, "PxArticulationLink");
  f.object(103, 2, "PxShape");
  f.pose(40, {2, 4, 6});
  f.vector(40, "LinearVelocity", {});
  f.vector(40, "AngularVelocity", {0, 0, 3});
  f.attachment(40, 1, 103, 2, {{0, 0, 0}, {1, 1, 1}},
               ActorKind::articulation_link);
  result = build_body_overlays(f.frame, f.snapshot, f.all());
  check(result.complete && result.angular_vectors == 1 &&
            result.linear_vectors == 0,
        "captured articulation-link fields are supported; zero vectors draw "
        "nothing");
  close(result.angular_velocity[0].position1, {2, 4, 6.6f},
        "link angular world vector");
  auto options = f.all();
  options.velocity_scale = 0;
  f.snapshot.instances.at(40).properties.clear();
  result = build_body_overlays(f.frame, f.snapshot, options);
  check(result.complete && result.bounds_count == 2 &&
            result.linear_velocity.empty() && result.angular_velocity.empty(),
        "scale zero disables vector work without disabling bounds");
}
void invalid() {
  {
    Fixture f;
    f.snapshot.event_count = 124;
    auto r = build_body_overlays(f.frame, f.snapshot, f.all());
    check(!r.complete && has(r, BodyOverlayCode::snapshot_mismatch) &&
              r.bounds.empty(),
          "different source prefixes never mix");
  }
  {
    Fixture f;
    ++f.snapshot.instances.at(f.a).generation;
    auto r = build_body_overlays(f.frame, f.snapshot, f.all());
    check(!r.complete && has(r, BodyOverlayCode::stale_source) &&
              r.bounds_count == 1 && r.linear_vectors == 1,
          "stale actor generation cannot draw bounds or vectors");
  }
  {
    Fixture f;
    f.object(2, 0, "PxScene");
    f.reference(f.a, "Scene", 2);
    auto r = build_body_overlays(f.frame, f.snapshot, f.all());
    check(!r.complete && has(r, BodyOverlayCode::stale_source) &&
              r.bounds_count == 1,
          "known foreign-scene attachment is omitted");
  }
  {
    Fixture f;
    f.frame.instances[0].world_bounds.reset();
    auto r = build_body_overlays(f.frame, f.snapshot, f.all());
    check(!r.complete && has(r, BodyOverlayCode::missing_bounds) &&
              r.bounds_count == 2 && r.linear_vectors == 2,
          "missing finite bounds are diagnostic, not invented");
  }
  for (int mode = 0; mode < 2; ++mode) {
    Fixture f;
    if (mode == 0)
      f.frame.instances[0].world_bounds->minimum.x = 100;
    else
      f.frame.instances[0].world_bounds->maximum.y =
          std::numeric_limits<float>::infinity();
    auto r = build_body_overlays(f.frame, f.snapshot, f.all());
    check(!r.complete && has(r, BodyOverlayCode::invalid_bounds) &&
              r.bounds_count == 2,
          "invalid AABB cannot partially emit edges");
  }
  {
    Fixture f;
    f.snapshot.instances.at(f.a).properties.erase("LinearVelocity");
    auto r = build_body_overlays(f.frame, f.snapshot, f.all());
    check(!r.complete && has(r, BodyOverlayCode::missing_property) &&
              r.linear_vectors == 1 && r.angular_vectors == 1 &&
              r.bounds_count == 3,
          "missing requested velocity has no pose-delta/other-actor fallback");
  }
  for (int mode = 0; mode < 5; ++mode) {
    Fixture f;
    if (mode == 0)
      f.put(f.a, "LinearVelocity", "PxVec3", Bytes(11));
    if (mode == 1)
      f.put(f.a, "LinearVelocity", "PvdF32", Bytes(12));
    if (mode == 2)
      f.vector(f.a, "LinearVelocity",
               {std::numeric_limits<float>::quiet_NaN(), 0, 0});
    if (mode == 3)
      f.put(f.a, "LinearVelocity", "PxVec3", Bytes(24), 2);
    if (mode == 4)
      f.snapshot.instances.at(f.a).properties.at("LinearVelocity").byte_order =
          static_cast<ByteOrder>(99);
    auto r = build_body_overlays(f.frame, f.snapshot, f.all());
    check(!r.complete && has(r, BodyOverlayCode::invalid_property) &&
              r.linear_vectors == 1 && r.angular_vectors == 1,
          "invalid typed velocity is isolated and does not throw");
  }
  for (int mode = 0; mode < 3; ++mode) {
    Fixture f;
    if (mode == 0)
      f.snapshot.instances.at(f.a).properties.erase("GlobalPose");
    if (mode == 1)
      f.put(f.a, "GlobalPose", "PxTransform", Bytes(27));
    if (mode == 2)
      f.floats(f.a, "GlobalPose", "PxTransform", {0, 0, 0, 2, 10, 20, 30});
    auto r = build_body_overlays(f.frame, f.snapshot, f.all());
    check(!r.complete && r.bounds_count == 3 && r.linear_vectors == 1 &&
              r.angular_vectors == 0,
          "invalid/missing actor pose never substitutes shape pose as vector "
          "anchor");
  }
  {
    Fixture f;
    const auto max = std::numeric_limits<float>::max();
    f.pose(f.a, {max, 20, 30});
    f.vector(f.a, "LinearVelocity", {max, 0, 0});
    auto options = f.all();
    options.velocity_scale = 2;
    auto r = build_body_overlays(f.frame, f.snapshot, options);
    check(!r.complete && has(r, BodyOverlayCode::invalid_property) &&
              r.linear_vectors == 1 && r.linear_velocity.size() == 3,
          "finite-input endpoint overflow rejects a whole arrow");
    for (const auto &line : r.angular_velocity)
      check(std::isfinite(line.position1.x) &&
                std::isfinite(line.position1.y) &&
                std::isfinite(line.position1.z),
            "all surviving arrow lines are finite");
  }
}
void budgets() {
  Fixture f;
  BodyOverlayOptions disabled;
  disabled.max_attachments = 0;
  auto off = build_body_overlays(f.frame, f.snapshot, disabled);
  check(off.complete && off.processed_attachments == 0 && off.bounds.empty() &&
            off.diagnostics.empty(),
        "disabled layers have no traversal even with zero work budget");
  auto flat_frame = f.frame;
  flat_frame.instances[0].world_bounds->maximum.z =
      flat_frame.instances[0].world_bounds->minimum.z;
  BodyOverlayOptions flat_options;
  flat_options.show_bounds = true;
  auto flat = build_body_overlays(flat_frame, f.snapshot, flat_options);
  check(flat.complete && flat.bounds_count == 3 &&
            flat.bounds[0].position0.z == 3 && flat.bounds[0].position1.z == 3,
        "zero extent AABB remains tight without invented inflation");
  auto o = f.all();
  o.max_lines = 15;
  auto r = build_body_overlays(f.frame, f.snapshot, o);
  check(!r.complete && has(r, BodyOverlayCode::limit_exceeded) &&
            r.bounds.size() == 12 && r.linear_velocity.size() == 3 &&
            r.angular_velocity.empty(),
        "shared line budget commits only complete AABB/arrow primitives");
  o = f.all();
  o.max_lines = 11;
  r = build_body_overlays(f.frame, f.snapshot, o);
  check(!r.complete && r.bounds.empty() && r.linear_velocity.empty(),
        "no partial AABB on budget failure");
  o = {};
  o.show_linear_velocity = true;
  o.max_lines = 2;
  r = build_body_overlays(f.frame, f.snapshot, o);
  check(!r.complete && r.linear_velocity.empty(),
        "no partial arrow shaft on budget failure");
  o = f.all();
  o.max_attachments = 1;
  r = build_body_overlays(f.frame, f.snapshot, o);
  check(!r.complete && r.processed_attachments == 1 && r.bounds_count == 1,
        "work budget bounds examined attachments");
  o = f.all();
  o.max_diagnostics = 1;
  for (auto &shape : f.frame.instances)
    shape.world_bounds.reset();
  r = build_body_overlays(f.frame, f.snapshot, o);
  check(!r.complete && r.diagnostics.size() == 1,
        "diagnostic retention remains bounded");
  for (int bad = 0; bad < 3; ++bad) {
    o = {};
    if (bad == 0)
      o.velocity_scale = -1;
    if (bad == 1)
      o.velocity_scale = std::numeric_limits<float>::quiet_NaN();
    if (bad == 2)
      o.max_diagnostics = 0;
    bool threw = false;
    try {
      build_body_overlays(f.frame, f.snapshot, o);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    check(threw, "invalid options rejected");
  }
}
void sdk(const std::string &path) {
  auto session = Session::load_file(path);
  Extractor extractor;
  std::size_t arrows = 0, boxes = 0, frames = 0;
  for (const auto &marker : session.frames()) {
    auto snapshot = session.snapshot_at_event(marker.event_count);
    ExtractOptions selected;
    selected.scene_id = marker.scene_id;
    auto frame = extractor.extract(session.metadata(), snapshot, selected);
    BodyOverlayOptions options;
    options.show_bounds = options.show_linear_velocity =
        options.show_angular_velocity = true;
    options.visible_attachments.emplace();
    for (const auto &shape : frame.instances)
      if (shape.actor.kind != ActorKind::rigid_static)
        options.visible_attachments->push_back(shape.picking_key());
    auto result = build_body_overlays(frame, snapshot, options);
    if (!result.complete)
      for (const auto &d : result.diagnostics)
        std::cerr << d.property << ": " << d.message << '\n';
    check(result.complete,
          "real SDK dynamic body fields must produce valid requested overlays");
    check(result.bounds_count == options.visible_attachments->size(),
          "real SDK tight bounds per selected dynamic attachment");
    for (const auto &line : result.linear_velocity)
      if (line.part == BodyOverlayPart::vector_shaft) {
        const auto *object = snapshot.instance(line.source.key.actor.id);
        const auto pose = object->property("GlobalPose")->as_transform();
        const auto velocity = object->property("LinearVelocity")->as_vec3();
        check(object->generation == line.source.key.actor.generation,
              "real SDK actor source generation");
        close(line.position0, pose->position,
              "real SDK captured actor position");
        close(line.position1,
              {pose->position.x + velocity->x * .2f,
               pose->position.y + velocity->y * .2f,
               pose->position.z + velocity->z * .2f},
              "real SDK captured linear velocity");
      }
    arrows += result.linear_vectors + result.angular_vectors;
    boxes += result.bounds_count;
    ++frames;
  }
  check(frames > 2 && arrows > 0 && boxes > 0,
        "real SDK body overlay coverage");
  std::cout << "SDK body overlays: " << frames << " frames, " << boxes
            << " tight boxes, " << arrows << " vectors\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    for (auto order : {ByteOrder::little, ByteOrder::big})
      for (auto width : {std::uint8_t{4}, std::uint8_t{8}})
        basic(order, width);
    visibility();
    static_links_zero_scale();
    invalid();
    budgets();
    if (argc > 1)
      sdk(argv[1]);
    std::cout << "body overlay tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
