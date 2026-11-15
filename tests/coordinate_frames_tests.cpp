#include "pvd/coordinate_frames.hpp"
#include "pvd/coordinates.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace r = pvd::render;
bool testing_view = false;
r::CoordinateFrames checked_coordinate_frames(
    const r::Frame &frame, const Snapshot &snapshot,
    const r::CoordinateFrameOptions &options) {
  if (testing_view)
    return r::build_coordinate_frames(frame, SnapshotView(snapshot), options);
  return r::build_coordinate_frames(frame, snapshot, options);
}
void check(bool v, const char *message) {
  if (!v)
    throw std::runtime_error(message);
}
void near(Vec3 a, Vec3 b, const char *message) {
  check(std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f &&
            std::abs(a.z - b.z) < 1e-4f,
        message);
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  std::size_t before = 0, shifted = 0;
  void emit(Payload v) {
    events.push_back({1, events.size(), events.size() * 7, std::move(v)});
  }
  std::uint32_t text(const std::string &s) {
    auto f = handles.find(s);
    if (f != handles.end())
      return f->second;
    auto n = static_cast<std::uint32_t>(handles.size() + 1);
    handles[s] = n;
    emit(StringHandleEvent{s, n});
    return n;
  }
  WireName type(const char *n) {
    auto ns = text("physx3"), name = text(n);
    return {ns, name};
  }
  void prop(const char *cls, const char *name, const char *t) {
    auto c = type(cls), dt = type(t);
    emit(CreateProperty{c, text(name), 0, dt, 1, {}});
  }
  void create(const char *cls, std::uint64_t id) {
    emit(CreateInstance{type(cls), id});
  }
  Bytes integer(std::uint64_t n, std::size_t width) {
    Bytes b;
    for (std::size_t i = 0; i < width; ++i)
      b.push_back(static_cast<std::uint8_t>(
          n >>
          (8 * (header.byte_order == ByteOrder::little ? i : width - i - 1))));
    return b;
  }
  Bytes floats(std::initializer_list<float> values) {
    Bytes b;
    for (auto f : values) {
      std::uint32_t bits;
      std::memcpy(&bits, &f, 4);
      auto part = integer(bits, 4);
      b.insert(b.end(), part.begin(), part.end());
    }
    return b;
  }
  void value(std::uint64_t id, const char *name, const char *t, Bytes b) {
    auto h = text(name);
    auto dt = type(t);
    emit(SetPropertyValue{id, h, dt, 1, std::move(b)});
  }
  void ref(std::uint64_t id, const char *name, std::uint64_t to) {
    value(id, name, "VoidPtr", integer(to, header.pointer_width()));
  }
  Fixture(ByteOrder order, std::uint8_t width) {
    header.byte_order = order;
    header.stream_flags = width == 8 ? 1 : 0;
    for (const char *cls : {"PxScene", "PxRigidDynamic", "PxRigidStatic",
                            "PxArticulation", "PxArticulationLink", "PxShape"})
      emit(CreateClass{type(cls)});
    for (const char *cls :
         {"PxRigidDynamic", "PxRigidStatic", "PxArticulation"})
      prop(cls, "Scene", "ObjectRef");
    for (const char *cls :
         {"PxRigidDynamic", "PxRigidStatic", "PxArticulationLink"})
      prop(cls, "GlobalPose", "PxTransform");
    for (const char *cls : {"PxRigidDynamic", "PxArticulationLink"})
      prop(cls, "CMassLocalPose", "PxTransform");
    prop("PxArticulationLink", "Parent", "ObjectRef");
    create("PxScene", 1);
    create("PxScene", 2);
    create("PxRigidDynamic", 10);
    create("PxRigidStatic", 11);
    create("PxArticulation", 20);
    create("PxArticulationLink", 21);
    for (auto id : {100u, 101u, 102u, 103u})
      create("PxShape", id);
    ref(10, "Scene", 1);
    ref(11, "Scene", 2);
    ref(20, "Scene", 1);
    ref(21, "Parent", 20);
    const float q = .70710678118f;
    value(10, "GlobalPose", "PxTransform", floats({0, 0, q, q, 10, 20, 30}));
    value(10, "CMassLocalPose", "PxTransform", floats({q, 0, 0, q, 1, 0, 0}));
    value(11, "GlobalPose", "PxTransform", floats({0, 0, 0, 1, 100, 200, 300}));
    value(21, "GlobalPose", "PxTransform", floats({0, 0, 0, 1, 5, 6, 7}));
    value(21, "CMassLocalPose", "PxTransform", floats({0, 0, 0, 1, 0, 2, 0}));
    before = events.size();
    emit(OriginShift{1, {2, 3, 4}});
    shifted = events.size();
    emit(DestroyInstance{10});
    create("PxRigidDynamic", 10);
    ref(10, "Scene", 1);
    value(10, "GlobalPose", "PxTransform", floats({0, 0, 0, 1, 1, 2, 3}));
  }
  r::Frame frame(const Snapshot &snapshot) {
    r::Frame f;
    f.event_count = snapshot.event_count;
    auto shape = [&](std::uint64_t scene, std::uint64_t actor, std::uint64_t id,
                     r::ActorKind kind, Vec3 p) {
      const auto *s = snapshot.instance(scene);
      const auto *a = snapshot.instance(actor);
      const auto *sh = snapshot.instance(id);
      r::ShapeInstance item;
      item.scene_id = scene;
      item.scene_generation = s->generation;
      item.actor_id = actor;
      item.actor_generation = a->generation;
      item.shape_id = id;
      item.shape_generation = sh->generation;
      item.actor.kind = kind;
      item.world_pose.position = p;
      f.instances.push_back(item);
    };
    shape(1, 10, 100, r::ActorKind::rigid_dynamic, {50, 60, 70});
    shape(1, 10, 101, r::ActorKind::rigid_dynamic, {51, 61, 71});
    shape(2, 11, 102, r::ActorKind::rigid_static, {100, 200, 300});
    shape(1, 21, 103, r::ActorKind::articulation_link, {5, 6, 7});
    return f;
  }
};
void run(ByteOrder order, std::uint8_t width) {
  Fixture f(order, width);
  Session session(f.header, f.events);
  auto snapshot = session.snapshot_at_event(f.shifted);
  auto frame = f.frame(snapshot);
  auto raw = snapshot.instance(10)->property("GlobalPose")->raw_bytes();
  r::CoordinateFrameOptions options;
  options.actor = options.shape = options.mass = true;
  options.axis_length = 2;
  auto lines = checked_coordinate_frames(frame, snapshot, options);
  check(lines.complete && lines.actor_count == 3 && lines.shape_count == 4 &&
            lines.mass_count == 2 && lines.actor.size() == 9 &&
            lines.shape.size() == 12 && lines.mass.size() == 6,
        "distinct actor/shape/mass triads and duplicate actor dedup");
  near(lines.actor[0].position0, {8, 17, 26},
       "actor origin follows native retained OriginShift value");
  near(lines.actor[0].position1, {8, 19, 26},
       "actor quaternion rotates local X axis");
  near(lines.mass[0].position0, {8, 18, 26},
       "mass origin composes nonidentity local COM offset");
  near(lines.mass[1].position1, {8, 18, 28},
       "mass orientation composes actor Z and local X rotations");
  near(lines.shape[0].position0, {50, 60, 70},
       "shape frame uses supplied native attachment world pose");
  check(lines.mass[0].source.key.actor.id == 10 &&
            lines.mass[3].source.key.actor.id == 21 &&
            lines.mass[0].source.snapshot_event_count == snapshot.event_count,
        "real source keys and articulation-link mass frame");
  check(snapshot.instance(10)->property("GlobalPose")->raw_bytes() == raw,
        "frame building preserves captured bytes");
  options.attachments =
      std::vector<r::PickingKey>{frame.instances[1].picking_key()};
  auto only = checked_coordinate_frames(frame, snapshot, options);
  check(only.complete && only.actor_count == 1 && only.shape_count == 1 &&
            only.mass_count == 1 && only.mass[0].source.key.shape.id == 101,
        "exact attachment filter chooses legitimate shared actor source");
  options.attachments = std::vector<r::PickingKey>{};
  auto none = checked_coordinate_frames(frame, snapshot, options);
  check(none.complete && none.diagnostics.empty() &&
            none.processed_attachments == 0,
        "hidden empty mask performs no source inspection");
  options.attachments.reset();
  options.max_lines = 2;
  auto budget = checked_coordinate_frames(frame, snapshot, options);
  check(!budget.complete && budget.actor.empty() && budget.shape.empty() &&
            budget.mass.empty(),
        "triad budget is atomic across all families");
  options.max_lines = 6;
  options.max_diagnostics = 1;
  auto bounded = checked_coordinate_frames(frame, snapshot, options);
  check(!bounded.complete &&
            bounded.shape.size() + bounded.actor.size() + bounded.mass.size() ==
                6 &&
            bounded.diagnostics.size() == 1,
        "shared budget and bounded diagnostics");
  options.max_lines = 100;
  options.max_diagnostics = 256;
  options.max_attachments = 1;
  auto work = checked_coordinate_frames(frame, snapshot, options);
  check(!work.complete && work.processed_attachments == 1,
        "attachment work budget");
  options.max_attachments = 100;
  auto latest = session.latest();
  auto stale = checked_coordinate_frames(frame, latest, options);
  check(!stale.complete && stale.diagnostics[0].code ==
                               r::CoordinateFrameCode::snapshot_mismatch,
        "mismatched prefixes rejected");
  frame.event_count = latest.event_count;
  stale = checked_coordinate_frames(frame, latest, options);
  check(!stale.complete &&
            stale.diagnostics[0].code == r::CoordinateFrameCode::stale_source,
        "reused actor generation cannot inherit old triad");
  auto current = f.frame(latest);
  options.shape = options.actor = false;
  auto missing = checked_coordinate_frames(current, latest, options);
  check(
      !missing.complete && missing.mass_count == 1 &&
          missing.diagnostics[0].property == "CMassLocalPose",
      "missing dynamic COM is diagnostic, static has no fabricated mass frame");
  auto damaged = snapshot;
  auto *p = &damaged.instances.at(10).properties.at("CMassLocalPose");
  p->bytes = std::make_shared<const Bytes>(std::size_t(1), std::uint8_t(0));
  frame = f.frame(snapshot);
  auto invalid = checked_coordinate_frames(frame, damaged, options);
  check(!invalid.complete && invalid.mass_count == 1 &&
            invalid.diagnostics[0].code ==
                r::CoordinateFrameCode::invalid_property,
        "malformed local COM isolated");
  auto foreign = snapshot;
  auto &scene_reference = foreign.instances.at(10).properties.at("Scene");
  scene_reference.bytes = std::make_shared<const Bytes>(f.integer(2, width));
  scene_reference.reference_ids = std::vector<std::uint64_t>{2};
  options.attachments =
      std::vector<r::PickingKey>{frame.instances[0].picking_key()};
  const auto wrong_scene = checked_coordinate_frames(frame, foreign, options);
  check(!wrong_scene.complete && wrong_scene.mass.empty() &&
            wrong_scene.diagnostics[0].property == "Scene",
        "foreign-scene actor cannot borrow the supplied source scene");
  auto unknown = frame;
  unknown.instances[0].actor.kind = static_cast<r::ActorKind>(99);
  const auto wrong_kind =
      checked_coordinate_frames(unknown, snapshot, options);
  check(!wrong_kind.complete && wrong_kind.mass.empty() &&
            wrong_kind.diagnostics[0].property == "actor.kind",
        "unknown actor kind is explicit rather than treated as articulation");
  options.attachments.reset();
  options.axis_length = 0;
  auto zero = checked_coordinate_frames(frame, damaged, options);
  check(zero.complete && zero.diagnostics.empty(),
        "zero scale disables querying");
  auto before = session.snapshot_at_event(f.before);
  options.axis_length = 2;
  options.actor = true;
  options.mass = false;
  auto reverse = checked_coordinate_frames(f.frame(before), before, options);
  near(reverse.actor[0].position0, {10, 20, 30},
       "reverse seek restores original native frame without mutating source");
}
using Matrix = std::array<std::array<double, 3>, 3>;
Matrix rotation(const std::vector<float> &p) {
  const double x = p.at(0), y = p.at(1), z = p.at(2), w = p.at(3);
  return {
      {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
       {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
       {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}
Vec3 matrix_vector(const Matrix &m, Vec3 p) {
  return {float(m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z),
          float(m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z),
          float(m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z)};
}
void real(const char *capture, const char *csv_path) {
  using Key = std::pair<std::uint64_t, std::string>;
  std::map<unsigned, std::map<Key, std::vector<float>>> fields;
  std::ifstream csv(csv_path);
  check(bool(csv), "real SDK oracle CSV");
  std::string line;
  std::getline(csv, line);
  while (std::getline(csv, line)) {
    std::istringstream input(line);
    std::vector<std::string> row;
    std::string value;
    while (std::getline(input, value, ','))
      row.push_back(value);
    check(row.size() == 8, "real SDK articulation CSV schema");
    if (row[1] != "post1" || row[6] != "f" ||
        (row[4] != "GlobalPose" && row[4] != "CMassLocalPose" &&
         row[4] != "@WorldPose"))
      continue;
    auto &values = fields[static_cast<unsigned>(std::stoul(row[0]))]
                         [{std::stoull(row[2]), row[4]}];
    const auto component = static_cast<std::size_t>(std::stoul(row[5]));
    if (values.size() <= component)
      values.resize(component + 1);
    values[component] = std::stof(row[7]);
  }
  auto session = Session::load_file(capture);
  std::map<std::uint64_t, unsigned> scenes;
  for (const auto &frame : session.frames())
    scenes.emplace(frame.scene_id, static_cast<unsigned>(scenes.size()));
  check(scenes.size() == 2 && fields.size() == 10,
        "real SDK dual-scene ten-step oracle");
  std::vector<std::size_t> prefixes;
  for (const auto &section : session.sections())
    if (section.name == "validation_after_fetch" &&
        scenes.at(section.section_id) == 1)
      prefixes.push_back(section.event_count);
  check(prefixes.size() == 10, "explicit after-fetch event prefixes");
  r::Extractor extractor;
  std::size_t checked = 0;
  for (bool reverse : {false, true})
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
      const auto ordinal = reverse ? prefixes.size() - 1 - i : i;
      const auto step = static_cast<unsigned>(ordinal + 1);
      auto snapshot = session.snapshot_at_event(prefixes[ordinal]);
      auto frame = extractor.extract(session.metadata(), snapshot);
      r::CoordinateFrameOptions options;
      options.actor = options.shape = options.mass = true;
      options.axis_length = 2;
      auto result = checked_coordinate_frames(frame, snapshot, options);
      check(result.complete && result.actor_count == 9 &&
                result.shape_count == 9 && result.mass_count == 8,
            "real SDK native classes and captured COM fields");
      for (const auto *family : {&result.actor, &result.shape, &result.mass})
        for (const auto &axis : *family) {
          const auto actor = axis.source.key.actor.id,
                     shape = axis.source.key.shape.id;
          const auto &source = fields.at(step).at(
              {axis.source.kind == r::CoordinateFrameKind::shape ? shape
                                                                 : actor,
               axis.source.kind == r::CoordinateFrameKind::shape
                   ? "@WorldPose"
                   : "GlobalPose"});
          check(source.size() == 7, "SDK getter pose components");
          auto matrix = rotation(source);
          Vec3 origin{source[4], source[5], source[6]};
          if (axis.source.kind == r::CoordinateFrameKind::mass) {
            const auto &local = fields.at(step).at({actor, "CMassLocalPose"});
            check(local.size() == 7, "SDK CMass getter components");
            const auto offset =
                matrix_vector(matrix, {local[4], local[5], local[6]});
            origin = {origin.x + offset.x, origin.y + offset.y,
                      origin.z + offset.z};
            const auto right = rotation(local);
            Matrix combined{};
            for (unsigned row = 0; row < 3; ++row)
              for (unsigned column = 0; column < 3; ++column)
                for (unsigned k = 0; k < 3; ++k)
                  combined[row][column] += matrix[row][k] * right[k][column];
            matrix = combined;
          }
          Vec3 local{};
          if (axis.axis == 0)
            local.x = 2;
          else if (axis.axis == 1)
            local.y = 2;
          else
            local.z = 2;
          const auto delta = matrix_vector(matrix, local);
          near(axis.position0, origin, "real SDK getter frame origin");
          near(axis.position1,
               {origin.x + delta.x, origin.y + delta.y, origin.z + delta.z},
               "real SDK independent matrix-composed frame axis");
          ++checked;
        }
    }
  std::cout << "Real SDK actor/shape/mass axes: " << checked
            << " forward/reverse getter comparisons passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    for (bool view : {false, true}) {
      testing_view = view;
      for (auto order : {ByteOrder::little, ByteOrder::big})
        for (auto width : {std::uint8_t(4), std::uint8_t(8)})
          run(order, width);
      if (argc == 3)
        real(argv[1], argv[2]);
      else
        check(argc == 1, "usage: coordinate_frames_tests "
                         "[articulation_capture.pxd2 expected.csv]");
    }
    std::cout << "Coordinate frames Snapshot/SnapshotView native/COM/attachment/generation/budgets "
                 "LE/BE 32/64 passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
