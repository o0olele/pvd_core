#include "pvd/render.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
using Row = std::vector<std::string>;
using Sample = std::pair<unsigned, std::string>;
void need(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
Row split(const std::string &line) {
  Row r;
  std::size_t at = 0;
  for (;;) {
    auto end = line.find(',', at);
    r.push_back(line.substr(at, end - at));
    if (end == std::string::npos)
      break;
    at = end + 1;
  }
  return r;
}
std::uint64_t u(const Row &row, std::size_t at) {
  return std::stoull(row.at(at));
}
float number(const PropertyValue &value, unsigned component) {
  const auto &bytes = value.raw_bytes();
  need(std::size_t(component) * 4 + 4 <= bytes.size(),
       "float component available");
  std::uint32_t bits = 0;
  for (unsigned i = 0; i < 4; ++i)
    bits |= std::uint32_t(bytes[std::size_t(component) * 4 + i])
            << ((value.byte_order == ByteOrder::little ? i : 3 - i) * 8);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
bool close(float a, float b) {
  return std::isfinite(a) && std::isfinite(b) &&
         std::abs(a - b) <= 2e-6f + std::abs(b) * 3e-6f;
}
std::vector<std::uint64_t> refs(const InstanceState &object,
                                const std::string &property) {
  const auto *p = object.property(property);
  return p && p->reference_ids ? *p->reference_ids
                               : std::vector<std::uint64_t>{};
}
std::map<Sample, std::vector<Row>> read(const std::string &path) {
  std::ifstream file(path);
  need(bool(file), "CSV input");
  std::map<Sample, std::vector<Row>> result;
  std::string line;
  std::getline(file, line);
  while (std::getline(file, line)) {
    auto row = split(line);
    need(row.size() == 8, "CSV schema");
    result[{static_cast<unsigned>(u(row, 0)), row[1]}].push_back(
        std::move(row));
  }
  return result;
}

using Field = std::tuple<std::uint64_t, std::string, unsigned>;
using Fields = std::map<Field, Row>;
struct Oracle {
  std::map<Sample, std::vector<Row>> rows;
  std::map<Sample, Fields> fields;
  explicit Oracle(const std::string &path) : rows(read(path)) {
    for (const auto &sample : rows)
      for (const auto &row : sample.second)
        fields[sample.first]
              [{u(row, 2), row[4], static_cast<unsigned>(u(row, 5))}] = row;
  }
  const Row &at(Sample sample, std::uint64_t object, const std::string &name,
                unsigned component = 0) const {
    return fields.at(sample).at({object, name, component});
  }
  std::uint64_t integer(Sample sample, std::uint64_t object,
                        const std::string &name) const {
    return u(at(sample, object, name), 7);
  }
  std::set<std::uint64_t> references(Sample sample, std::uint64_t object,
                                     const std::string &name) const {
    std::set<std::uint64_t> out;
    for (const auto &row : rows.at(sample))
      if (u(row, 2) == object && row[4] == name && row[6] == "r")
        out.insert(u(row, 7));
    return out;
  }
};
struct Check {
  const Session &session;
  const Oracle &oracle;
  std::map<Sample, std::size_t> prefixes;
  std::map<Sample, Snapshot> snapshots;
  std::map<std::uint64_t, unsigned> scenes;
  std::size_t checked = 0, unrecorded = 0, deferred = 0, duplicate_arrays = 0,
              rendered = 0;
  render::Extractor extractor;
  Check(const Session &s, const Oracle &o) : session(s), oracle(o) {
    unsigned post[2]{};
    for (const auto &f : session.frames()) {
      auto [it, added] =
          scenes.emplace(f.scene_id, static_cast<unsigned>(scenes.size()));
      (void)added;
      prefixes[{static_cast<unsigned>(f.index + 1),
                it->second ? "frame1" : "frame0"}] = f.event_count;
    }
    for (const auto &section : session.sections()) {
      if (section.name == "articulation_initial")
        prefixes[{0, "initial"}] = section.event_count;
      else if (section.name == "validation_after_fetch") {
        unsigned scene = scenes.at(section.section_id);
        prefixes[{++post[scene], scene ? "post1" : "post0"}] =
            section.event_count;
      }
    }
    need(prefixes.size() == 41 && oracle.rows.size() == 41 &&
             scenes.size() == 2,
         "initial+10 dual-scene native and post-fetch samples");
    for (const auto &sample : prefixes)
      snapshots.emplace(sample.first, session.snapshot_at_event(sample.second));
  }
  unsigned source_scene(Sample sample, std::uint64_t object) const {
    const auto &fields = oracle.fields.at(sample);
    auto shape = fields.find({object, "@Scene", 0});
    const auto id = shape != fields.end()
                        ? u(shape->second, 7)
                        : oracle.integer(sample, object, "Scene");
    return scenes.at(id);
  }
  bool initial_unsimulated(Sample sample, unsigned scene) const {
    return sample.first == 0 ||
           (sample.first == 1 &&
            (sample.second == "frame0" || sample.second == "post0") &&
            scene == 1);
  }
  bool unstepped_creation(Sample sample, std::uint64_t object) const {
    if (initial_unsimulated(sample, source_scene(sample, object)))
      return true;
    if (source_scene(sample, object) != 1 ||
        (sample.second != "frame0" && sample.second != "post0"))
      return false;
    const auto previous = Sample{sample.first - 1, "post1"};
    const auto *before = snapshots.at(previous).instance(object);
    const auto *current = snapshots.at(sample).instance(object);
    return current && (!before || before->generation != current->generation);
  }
  Sample motion_sample(Sample sample, std::uint64_t actor) const {
    if (sample.second != "frame0" && sample.second != "frame1")
      return sample;
    const unsigned scene = source_scene(sample, actor),
                   native = sample.second == "frame0" ? 0 : 1;
    if (scene != native)
      return sample;
    Sample previous = sample.first == 1 ? Sample{0, "initial"}
                                        : Sample{sample.first - 1, "post1"};
    const auto *before = snapshots.at(previous).instance(actor);
    const auto *current = snapshots.at(sample).instance(actor);
    return before && current && before->generation == current->generation
               ? previous
               : sample;
  }
  Row expected(Sample sample, const Row &row) const {
    const auto object = u(row, 2);
    const auto &cls = row[3];
    const auto &property = row[4];
    auto result = row;
    const bool sleep = property == "IsSleeping";
    if ((cls == "PxArticulation" || cls == "PxRigidDynamic") && sleep &&
        unstepped_creation(sample, object)) {
      result[7] = "1";
      return result; // Creation observes free-standing sleep before insertion
                     // wakes the SDK object.
    }
    if (cls == "PxArticulation" && (property == "WakeCounter" || sleep) &&
        (sample.second == "frame0" || sample.second == "frame1") &&
        source_scene(sample, object) == (sample.second == "frame0" ? 0u : 1u))
      return oracle.at(
          {sample.first, sample.second == "frame0" ? "post0" : "post1"}, object,
          property, static_cast<unsigned>(u(row, 5)));
    const bool motion =
        property == "GlobalPose" || property == "LinearVelocity" ||
        property == "AngularVelocity" || (cls == "PxRigidDynamic" && sleep);
    if (motion) {
      auto selected = motion_sample(sample, object);
      if (sleep && selected.first == 0) {
        result[7] = "1";
        return result;
      }
      if (selected == sample && sleep &&
          (sample.second == "frame0" || sample.second == "frame1") &&
          source_scene(sample, object) ==
              (sample.second == "frame0" ? 0u : 1u)) {
        result[7] = "1";
        return result;
      }
      return oracle.at(selected, object, property,
                       static_cast<unsigned>(u(row, 5)));
    }
    return result;
  }
  void sample(Sample sample) {
    const auto &snapshot = snapshots.at(sample);
    std::set<std::uint64_t> expected_objects, actual_objects;
    for (const auto &row : oracle.rows.at(sample))
      if (row[4] == "@Present")
        expected_objects.insert(u(row, 2));
    for (const auto &entry : snapshot.instances) {
      const auto &cls = entry.second.class_name.name;
      if (cls == "PxArticulation" || cls == "PxArticulationLink" ||
          cls == "PxArticulationJoint" || cls == "PxAggregate" ||
          cls == "PxRigidStatic" || cls == "PxRigidDynamic" || cls == "PxShape")
        actual_objects.insert(entry.first);
    }
    need(actual_objects == expected_objects,
         "released native owners/links/joints/shapes cannot survive as fake "
         "live objects");
    const auto closure = [&](std::uint64_t articulation) {
      std::set<std::uint64_t> result;
      auto pending = refs(*snapshot.instance(articulation), "Links");
      while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        need(result.insert(id).second,
             "native link graph is acyclic and has a single parent");
        const auto *link = snapshot.instance(id);
        need(link && link->class_name.name == "PxArticulationLink",
             "native link collection resolves actual links");
        auto children = refs(*link, "Links");
        pending.insert(pending.end(), children.begin(), children.end());
      }
      return result;
    };
    for (const auto &row : oracle.rows.at(sample))
      if (row[4] == "@AllLinks.@count") {
        const auto id = u(row, 2);
        need(closure(id) == oracle.references(sample, id, "@AllLinks"),
             "root/children graph closes over every actual SDK link");
      } else if (row[4] == "@AllActors.@count" &&
                 !unstepped_creation(sample, u(row, 2))) {
        const auto id = u(row, 2);
        const auto *aggregate = snapshot.instance(id);
        auto raw = refs(*aggregate, "Actors");
        std::set<std::uint64_t> members(raw.begin(), raw.end());
        for (auto owner : refs(*aggregate, "Articulations")) {
          auto links = closure(owner);
          members.insert(links.begin(), links.end());
        }
        need(members == oracle.references(sample, id, "@AllActors"),
             "aggregate membership counts all SDK links plus ordinary actors "
             "exactly once");
      }
    for (const auto &original : oracle.rows.at(sample)) {
      const auto object_id = u(original, 2);
      const auto &name = original[4];
      if (name[0] == '@')
        continue;
      const auto *object = snapshot.instance(object_id);
      need(object && object->class_name == QualifiedName{"physx3", original[3]},
           "actual captured object class/lifetime");
      if (name == "Aggregate" ||
          (original[3] == "PxArticulationLink" && name == "Scene")) {
        need(!object->property(name),
             "native excluded ownership property must not be fabricated");
        ++unrecorded;
        continue;
      }
      if (original[6] == "c") {
        auto field = name.substr(0, name.size() - 7);
        auto expected_set = oracle.references(sample, object_id, field);
        auto raw = refs(*object, field);
        std::set<std::uint64_t> actual(raw.begin(), raw.end());
        if (raw.size() != actual.size()) {
          need(original[3] == "PxArticulationLink" && field == "Shapes",
               "only known native link.Shapes duplication is canonicalized");
          ++duplicate_arrays;
        }
        if (original[3] == "PxAggregate" &&
            unstepped_creation(sample, object_id)) {
          need(std::includes(expected_set.begin(), expected_set.end(),
                             actual.begin(), actual.end()),
               "deferred initial membership cannot invent members");
          ++deferred;
        } else
          need(actual == expected_set,
               "native collection matches SDK unique membership: " + field);
        ++checked;
        continue;
      }
      if (original[6] == "r" &&
          oracle.fields.at(sample).count({object_id, name + ".@count", 0}))
        continue;
      const auto row = expected(sample, original);
      const auto *value = object->property(name);
      const auto context = std::to_string(sample.first) + " " + sample.second +
                           " " + original[3] + " " + name;
      if (row[6] == "r") {
        const auto expected_id = u(row, 7);
        need(expected_id == 0
                 ? !value || value->as_object_ref().value_or(
                                 value->as_uint().value_or(0)) == 0
                 : value && value->as_object_ref().value_or(
                                value->as_uint().value_or(0)) == expected_id,
             context + " reference");
      } else {
        need(value != nullptr, context + " captured property");
        if (row[6] == "u")
          need(value->as_uint().value_or(UINT64_MAX) == u(row, 7),
               context + " integer");
        else if (row[6] == "s")
          need(value->text.value_or("") == row[7], context + " string");
        else if (row[6] == "f") {
          const auto actual = number(*value, static_cast<unsigned>(u(row, 5))),
                     wanted = std::stof(row[7]);
          need(close(actual, wanted),
               context + " float " + std::to_string(actual) + " != " + row[7]);
        }
      }
      ++checked;
    }
    render::ExtractOptions all;
    auto frame = extractor.extract(session.metadata(), snapshot, all);
    need(frame.diagnostics.empty(),
         "actual articulation graph/render has no synthetic scene/actor "
         "fallback diagnostics");
    std::map<std::uint64_t, const render::ShapeInstance *> shapes;
    for (const auto &shape : frame.instances)
      need(shapes.emplace(shape.shape_id, &shape).second,
           "one unique SDK shape attachment per native link/actor");
    std::size_t expected_shapes = 0;
    for (const auto &original : oracle.rows.at(sample))
      if (original[4] == "@Actor") {
        ++expected_shapes;
        const auto shape_id = u(original, 2), actor_id = u(original, 7);
        const auto &shape = *shapes.at(shape_id);
        need(shape.actor_id == actor_id &&
                 shape.scene_id == oracle.integer(sample, shape_id, "@Scene"),
             "real scene/actor/shape identity");
        need(shape.actor_generation ==
                     snapshot.instance(actor_id)->generation &&
                 shape.shape_generation ==
                     snapshot.instance(shape_id)->generation &&
                 shape.scene_generation ==
                     snapshot.instance(shape.scene_id)->generation,
             "shape key retains source generations");
        const auto *actor = snapshot.instance(actor_id);
        const bool link = actor->class_name.name == "PxArticulationLink";
        need(shape.actor.kind == (link ? render::ActorKind::articulation_link
                                  : actor->class_name.name == "PxRigidDynamic"
                                      ? render::ActorKind::rigid_dynamic
                                      : render::ActorKind::rigid_static),
             "native actor kind");
        need(!shape.actor.kinematic && shape.actor.rigid_body_flags == 0,
             "articulation is not fabricated as kinematic/dynamic rigid actor");
        auto pose_sample = motion_sample(sample, actor_id);
        for (unsigned component = 0; component < 7; ++component) {
          const float wanted = std::stof(
              oracle.at(pose_sample, shape_id, "@WorldPose", component)[7]);
          const auto &t = shape.world_pose;
          const float actual[] = {t.rotation.x, t.rotation.y, t.rotation.z,
                                  t.rotation.w, t.position.x, t.position.y,
                                  t.position.z};
          need(close(actual[component], wanted),
               "SDK independent actor*shape world pose");
        }
        const auto *box = std::get_if<render::Box>(&shape.geometry);
        need(box && shape.world_bounds,
             "native SDK box shape and tight bounds");
        const float half[] = {box->half_extents.x, box->half_extents.y,
                              box->half_extents.z};
        const float lo[] = {shape.world_bounds->minimum.x,
                            shape.world_bounds->minimum.y,
                            shape.world_bounds->minimum.z};
        const float hi[] = {shape.world_bounds->maximum.x,
                            shape.world_bounds->maximum.y,
                            shape.world_bounds->maximum.z};
        for (unsigned component = 0; component < 3; ++component) {
          need(close(half[component],
                     std::stof(oracle.at(sample, shape_id, "@HalfExtents",
                                         component)[7])),
               "SDK box dimensions");
          need(close(lo[component],
                     std::stof(oracle.at(pose_sample, shape_id, "@BoundsMin",
                                         component)[7])) &&
                   close(hi[component],
                         std::stof(oracle.at(pose_sample, shape_id,
                                             "@BoundsMax", component)[7])),
               "SDK independent geometry world bounds");
        }
        if (link) {
          auto owner = actor;
          std::set<std::uint64_t> visited;
          while (owner->class_name.name != "PxArticulation") {
            need(visited.insert(owner->id).second,
                 "acyclic actual articulation Parent graph");
            auto parent = refs(*owner, "Parent");
            need(parent.size() == 1, "exact parent ownership");
            owner = snapshot.instance(parent[0]);
            need(owner, "live parent lifetime");
          }
          need(shape.actor.sleeping ==
                   std::optional<bool>{
                       owner->property("IsSleeping")->as_uint().value_or(0) !=
                       0},
               "link sleep derives from its actual articulation owner");
        }
        ++rendered;
      }
    need(shapes.size() == expected_shapes && expected_shapes == 9,
         "no missing or fake shape instances");
    for (const auto &scene : scenes) {
      render::ExtractOptions selection;
      selection.scene_id = scene.first;
      auto filtered =
          extractor.extract(session.metadata(), snapshot, selection);
      std::size_t count = 0;
      for (const auto &row : oracle.rows.at(sample))
        if (row[4] == "@Scene" && row[6] == "r" && u(row, 7) == scene.first)
          ++count;
      need(filtered.diagnostics.empty() && filtered.instances.size() == count,
           "strict scene isolation via native scene/Parent ownership");
      for (const auto &shape : filtered.instances)
        need(shape.scene_id == scene.first,
             "foreign shape cannot leak through scene filter");
    }
  }
};
void run(const std::string &capture, const std::string &expected) {
  Oracle oracle(expected);
  for (unsigned pass = 0; pass < 2; ++pass) {
    SessionOptions options;
    options.checkpoint_interval = 32;
    options.max_checkpoints = pass ? 0 : 32;
    auto session = Session::load_file(capture, options);
    Check check(session, oracle);
    for (const auto &sample : check.prefixes)
      check.sample(sample.first);
    for (auto it = check.prefixes.rbegin(); it != check.prefixes.rend(); ++it)
      check.sample(it->first);
    need(check.unrecorded && check.deferred && check.duplicate_arrays,
         "known SDK ownership/cadence/duplicate behavior covered explicitly");
    std::size_t reused = 0;
    for (const auto &life : session.lifetimes())
      if (life.generation && (life.class_name.name == "PxAggregate" ||
                              life.class_name.name == "PxArticulation" ||
                              life.class_name.name == "PxArticulationLink"))
        ++reused;
    need(reused >= 1,
         "SDK remove/reinsert/recreate preserves actual reused-ID generations");
    std::cout << "Articulation replay checkpoints=" << options.max_checkpoints
              << ": " << check.checked << " fields / " << check.rendered
              << " attachments; omitted ownership=" << check.unrecorded
              << " deferred=" << check.deferred
              << " duplicate arrays=" << check.duplicate_arrays
              << " reused lifetimes=" << reused << '\n';
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    need(argc == 3,
         "usage: pvd_physx34_articulation capture.pxd2 expected.csv");
    run(argv[1], argv[2]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
