#include "pvd/coordinates.hpp"
#include "pvd/deformables.hpp"
#include "pvd/render.hpp"
#include "pvd/repx.hpp"
#include "pvd/scene_queries.hpp"
#include "pvd/writer.hpp"
#ifdef PVD_ORIGIN_TEST_ANALYSIS
#include "pvd/curves.hpp"
#endif
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
using Row = std::vector<std::string>;
void check(bool v, const std::string &m) {
  if (!v)
    throw std::runtime_error(m);
}
Row split(const std::string &line) {
  Row r;
  std::istringstream in(line);
  std::string s;
  while (std::getline(in, s, ','))
    r.push_back(s);
  return r;
}
std::uint64_t u(const std::string &s) { return std::stoull(s); }
float component(Transform t, const std::string &c) {
  if (c == "px")
    return t.position.x;
  if (c == "py")
    return t.position.y;
  if (c == "pz")
    return t.position.z;
  if (c == "qx")
    return t.rotation.x;
  if (c == "qy")
    return t.rotation.y;
  if (c == "qz")
    return t.rotation.z;
  if (c == "qw")
    return t.rotation.w;
  throw std::runtime_error("unknown transform component");
}
float component(Vec3 v, const std::string &c) {
  if (c == "x")
    return v.x;
  if (c == "y")
    return v.y;
  if (c == "z")
    return v.z;
  throw std::runtime_error("unknown vector component");
}
float scalar_at(const PropertyValue &v, std::size_t at) {
  const auto &bytes = coordinates::native_bytes(v);
  check(at + 4 <= bytes.size(), "float field bounds");
  std::uint32_t bits = 0;
  for (std::size_t i = 0; i < 4; ++i)
    bits |= std::uint32_t(bytes[at + i])
            << (8 * (v.byte_order == ByteOrder::little ? i : 3 - i));
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
std::string property(const Row &r) {
  if (r[5] == "LocalPose[0]")
    return "LocalPose[eACTOR0]";
  if (r[5] == "LocalPose[1]")
    return "LocalPose[eACTOR1]";
  return r[5];
}
std::size_t verify(const Session &s, const Snapshot &snapshot,
                   const std::vector<Row> &rows) {
  render::Extractor extractor;
  auto frame = extractor.extract(s.metadata(), snapshot);
  auto deformed = deformables::extract(s.metadata(), snapshot);
  std::size_t count = 0;
  for (const auto &r : rows) {
    if (r[3] == "query_input")
      continue;
    const auto id = u(r[2]);
    const auto *o = snapshot.instance(id);
    check(o, "SDK object missing: " + r[3]);
    const auto p = property(r), c = r[7];
    const auto index = static_cast<std::size_t>(u(r[6]));
    float actual = 0;
    if (p == "GlobalPose" || p.find("LocalPose[") == 0) {
      auto *v = o->property(p);
      check(v, "missing transform " + p);
      auto t = coordinates::native_transform(*v);
      check(bool(t), "transform type");
      actual = component(*t, c);
    } else if (p == "IsSleeping") {
      auto *v = o->property(p);
      check(v, "sleep flag");
      actual = float(*v->as_uint());
    } else if (p == "ParticleBuffer" || p == "@WorldParticle") {
      auto i = std::find_if(
          deformed.cloths.begin(), deformed.cloths.end(),
          [&](const auto &cloth) { return cloth.key.owner.id == id; });
      check(i != deformed.cloths.end(), "cloth extraction");
      const auto &vertices =
          p == "ParticleBuffer" ? i->local_positions : i->world_positions;
      check(index < vertices.size(), "cloth index");
      actual = component(vertices[index], c);
    } else if (p == "Positions") {
      auto i = std::find_if(
          deformed.particle_clouds.begin(), deformed.particle_clouds.end(),
          [&](const auto &cloud) { return cloud.key.owner.id == id; });
      check(i != deformed.particle_clouds.end(), "particle extraction");
      auto particle =
          std::find_if(i->particles.begin(), i->particles.end(),
                       [&](const auto &point) { return point.index == index; });
      check(particle != i->particles.end() && particle->position,
            "particle sparse index");
      actual = component(*particle->position, c);
    } else if (p.find("ProjectionPlane.") == 0) {
      auto *v = o->property("ProjectionPlane");
      check(v, "particle plane");
      actual = scalar_at(*v, p == "ProjectionPlane.distance" ? 12
                             : c == "x"                      ? 0
                             : c == "y"                      ? 4
                                                             : 8);
    } else {
      auto *v = o->property(p);
      check(v, "missing vector " + p);
      auto vector = v->as_vec3();
      check(bool(vector), "vector type");
      actual = component(*vector, c);
    }
    const float expected = std::stof(r[8]);
    check(actual == expected, r[0] + " " + r[3] + "." + p + "." + c + ": got " +
                                  std::to_string(actual) + " expected " + r[8]);
    if (p == "GlobalPose" && (c == "px" || c == "py" || c == "pz") &&
        r[4] != "PxCloth") {
      auto i =
          std::find_if(frame.instances.begin(), frame.instances.end(),
                       [&](const auto &shape) { return shape.actor_id == id; });
      check(i != frame.instances.end(), "render attachment missing");
      const auto value = c == "px"   ? i->world_pose.position.x
                         : c == "py" ? i->world_pose.position.y
                                     : i->world_pose.position.z;
      check(value == expected, "render pose differs from SDK getter");
    }
    ++count;
  }
  return count;
}
void run(const std::string &capture, const std::string &csv,
         const std::string &output) {
  std::ifstream in(std::filesystem::u8path(csv));
  check(bool(in), "CSV");
  std::string line;
  std::getline(in, line);
  check(line ==
            "phase,scene_id,object_id,name,kind,property,item,component,value",
        "CSV schema");
  std::map<std::string, std::vector<Row>> oracle;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto r = split(line);
    check(r.size() == 9, "CSV row");
    oracle[r[0]].push_back(std::move(r));
  }
  std::size_t checked = 0;
  for (auto interval : {std::size_t(7), std::size_t(0)}) {
    SessionOptions options;
    options.checkpoint_interval = interval;
    options.max_checkpoints = 7;
    auto s = Session::load_file(capture, options);
    std::vector<SectionMarker> markers;
    for (const auto &m : s.sections())
      if (m.name.find("origin_") == 0)
        markers.push_back(m);
    check(markers.size() >= 6, "SDK phase markers");
    for (bool reverse : {false, true}) {
      auto order = markers;
      if (reverse)
        std::reverse(order.begin(), order.end());
      for (const auto &m : order) {
        auto snapshot = s.snapshot_at_event(m.event_count);
        checked += verify(s, snapshot, oracle.at(m.name));
      }
    }
    const auto baseline = markers.front().event_count;
    const auto first =
        std::find_if(markers.begin(), markers.end(), [](const auto &m) {
          return m.name == "origin_shift1_immediate";
        })->event_count;
    const auto post =
        std::find_if(markers.begin(), markers.end(), [](const auto &m) {
          return m.name == "origin_shift1_post";
        })->event_count;
    const auto end = markers.back().event_count;
    auto state = s.snapshot_at_event(first);
    const auto id = u(oracle.at("origin_shift1_immediate").front()[2]);
#ifdef PVD_ORIGIN_TEST_ANALYSIS
    {
      namespace a = pvd::analysis;
      std::vector<SectionMarker> samples;
      for (const auto &m : s.sections())
        if (m.name == "validation_origin_sample")
          samples.push_back(m);
      check(samples.size() == markers.size(),
            "uniform SDK curve sample markers");
      const auto scene = markers.front().section_id;
      a::CurveRequest request;
      request.source.scene_id = scene;
      request.source.sampling = a::Sampling::after_fetch;
      request.source.after_fetch_section = "validation_origin_sample";
      request.property = "GlobalPose";
      request.component = "position.x";
      const auto last_state = s.snapshot_at_event(markers.back().event_count);
      for (const auto &[oid, o] : last_state.instances)
        if (o.property("GlobalPose")) {
          request.object = {oid, o.generation};
          auto native = a::sample_curve(s, request);
          check(native.samples.size() == markers.size(),
                "native SDK curve size");
          request.coordinate_space = a::CoordinateSpace::wire;
          auto wire = a::sample_curve(s, request);
          request.coordinate_space = a::CoordinateSpace::native;
          for (std::size_t n = 0; n < markers.size(); ++n) {
            const auto &rows = oracle.at(markers[n].name);
            auto found =
                std::find_if(rows.begin(), rows.end(), [&](const auto &r) {
                  return u(r[2]) == oid && r[5] == "GlobalPose" && r[7] == "px";
                });
            if (found == rows.end()) {
              check(!native.samples[n].value,
                    "new object curve did not retain gap");
              continue;
            }
            check(native.samples[n].value &&
                      a::plot_value(*native.samples[n].value) ==
                          std::stof((*found)[8]),
                  "native curve differs from SDK getter oracle");
            const auto raw_state = s.snapshot_at_event(samples[n].event_count);
            check(wire.samples[n].value &&
                      a::plot_value(*wire.samples[n].value) ==
                          raw_state.instance(oid)
                              ->property("GlobalPose")
                              ->as_transform()
                              ->position.x,
                  "wire curve no longer means raw source");
          }
        }
      const auto &last_rows = oracle.at(markers.back().name);
      auto particle =
          std::find_if(last_rows.begin(), last_rows.end(), [](const auto &r) {
            return r[5] == "ProjectionPlane.distance";
          });
      check(particle != last_rows.end(), "SDK particle-plane oracle");
      request.object = {u((*particle)[2]), 0};
      request.property = "ProjectionPlane";
      request.component = "w";
      const auto plane = a::sample_curve(s, request);
      for (std::size_t n = 0; n < markers.size(); ++n) {
        const auto &rows = oracle.at(markers[n].name);
        auto r = std::find_if(rows.begin(), rows.end(), [](const auto &row) {
          return row[5] == "ProjectionPlane.distance";
        });
        check(r != rows.end() && plane.samples[n].value &&
                  a::plot_value(*plane.samples[n].value) == std::stof((*r)[8]),
              "particle plane native curve differs from SDK getter");
      }
      auto native_diff = a::diff_events(s, baseline, first);
      auto changed =
          std::find_if(native_diff.objects.begin(), native_diff.objects.end(),
                       [&](const auto &v) { return v.object.id == id; });
      check(changed != native_diff.objects.end(),
            "native diff lost static origin movement");
      auto pose =
          std::find_if(changed->properties.begin(), changed->properties.end(),
                       [](const auto &p) { return p.name == "GlobalPose"; });
      check(pose != changed->properties.end() && pose->data_changed &&
                pose->coordinates_changed && !pose->wire_data_changed,
            "native diff fabricated a wire property write");
      a::DiffOptions wire_options;
      wire_options.coordinate_space = a::CoordinateSpace::wire;
      auto wire_diff = a::diff_events(s, baseline, first, wire_options);
      check(std::none_of(wire_diff.objects.begin(), wire_diff.objects.end(),
                         [&](const auto &v) { return v.object.id == id; }),
            "wire diff reported an untransmitted static pose change");
    }
#endif
    check(state.instance(id)
                  ->property("GlobalPose")
                  ->as_transform()
                  ->position.x == 100,
          "source raw pose was rewritten");
    check(coordinates::native_transform(
              *state.instance(id)->property("GlobalPose"))
                  ->position.x == 0,
          "native static coordinate");
    check(state.cameras.at("recorded_scene0").position.x == 100 &&
              state.cameras.at("recorded_scene1").position.x == 100,
          "unassociated named camera was guessed/shifted");
    auto queries = render::extract_scene_queries(
        s,
        render::SceneQueryWindow{markers.front().section_id, baseline, post});
    check(queries.queries.size() == 6 && queries.lines.empty(),
          "mixed-epoch query previews not suppressed");
    for (const auto &q : queries.queries)
      check(q.coordinates_ambiguous, "mixed query epoch missing");
    check(std::any_of(queries.diagnostics.begin(), queries.diagnostics.end(),
                      [](const auto &d) {
                        return d.code ==
                               render::SceneQueryCode::ambiguous_coordinates;
                      }),
          "query ambiguity diagnostic missing");
    // Saving retains original protocol bytes and effective coordinates.
    std::ostringstream encoded(std::ios::binary);
    writing::write_session(encoded, s);
    auto raw = encoded.str();
    auto saved = Session::parse(Bytes(raw.begin(), raw.end()), options);
    checked += verify(saved, saved.snapshot_at_event(post),
                      oracle.at("origin_shift1_post"));
    auto crop = writing::crop_events(s, first, end);
    Session cropped(crop.header, crop.events, options);
    const auto pending_marker = std::find_if(
        markers.begin(), markers.end(),
        [](const auto &m) { return m.name == "origin_rounding_post"; });
    if (pending_marker != markers.end() && pending_marker->event_count < end) {
      auto tail = writing::crop_events(s, pending_marker->event_count, end);
      Session resumed(tail.header, tail.events, options);
      const auto original_queries = render::extract_scene_queries(
          s, render::SceneQueryWindow{markers.front().section_id,
                                      pending_marker->event_count, end});
      const auto resumed_queries = render::extract_scene_queries(
          resumed, render::SceneQueryWindow{markers.front().section_id,
                                            tail.bootstrap_event_count,
                                            tail.events.size()});
      check(original_queries.queries.size() == 3 &&
                resumed_queries.queries.size() == 3,
            "pending collection continuation query count");
      for (std::size_t i = 0; i < 3; ++i)
        check(original_queries.queries[i].coordinates_ambiguous &&
                  resumed_queries.queries[i].coordinates_ambiguous,
              "crop lost the pending origin-shift query ambiguity");
      check(original_queries.lines.empty() && resumed_queries.lines.empty(),
            "pending ambiguous queries gained misleading previews after crop");
    }
    check(crop.events.size() - crop.bootstrap_event_count == end - first,
          "crop slice length");
    for (std::size_t i = first; i < end; ++i) {
      const auto &a = s.events()[i];
      const auto &b = crop.events[crop.bootstrap_event_count + i - first];
      check(a.stream_id == b.stream_id && a.timestamp == b.timestamp &&
                writing::encode_event(a, s.header().byte_order) ==
                    writing::encode_event(b, s.header().byte_order),
            "crop source slice lost wire fidelity");
    }
    for (const auto &m : markers)
      if (m.event_count >= first)
        checked += verify(cropped,
                          cropped.snapshot_at_event(crop.bootstrap_event_count +
                                                    m.event_count - first),
                          oracle.at(m.name));
    auto boot = cropped.snapshot_at_event(crop.bootstrap_event_count);
    check(
        boot.instance(id)->property("GlobalPose")->as_transform()->position.x ==
            0,
        "crop bootstrap should explicitly rebase world pose");
    if (!output.empty() && interval == 7) {
      // Select the native rigid/articulation components, excluding unsupported
      // cloth/particles explicitly rather than weakening strict export.
      repx::Options ro;
      ro.selection = repx::Selection::objects;
      auto final = s.snapshot_at_event(post);
      for (const auto &[oid, o] : final.instances)
        if (o.class_name == QualifiedName{"physx3", "PxRigidStatic"} ||
            o.class_name == QualifiedName{"physx3", "PxRigidDynamic"} ||
            o.class_name == QualifiedName{"physx3", "PxArticulation"})
          ro.objects.push_back({oid, o.generation});
      auto exported = repx::export_snapshot(s.metadata(), final, ro);
      check(exported.complete, "strict origin RepX export");
      std::ofstream xml(std::filesystem::u8path(output + ".repx"),
                        std::ios::binary);
      xml << exported.xml;
      check(bool(xml), "RepX output");
      std::ofstream map(std::filesystem::u8path(output + "_mapping.csv"),
                        std::ios::binary);
      map << "source_id,generation,serial_id,type,owner_serial_id,shape_"
             "index\n";
      for (const auto &o : exported.objects)
        map << o.source.id << ',' << o.source.generation << ',' << o.serial_id
            << ',' << o.type << ',' << o.owner_serial_id << ',' << o.shape_index
            << '\n';
      auto sdk_expected = [&](const std::string &prefix,
                              const repx::Result &result,
                              const std::string &phase) {
        std::ofstream expected(
            std::filesystem::u8path(prefix + "_sdk_expected.csv"),
            std::ios::binary);
        expected << "kind,serial_id,owner_id,shape_index,field,index,value\n"
                 << std::setprecision(9);
        for (const auto &m : result.objects) {
          expected << "object," << m.serial_id << ',' << m.owner_serial_id
                   << ',' << m.shape_index << ',' << m.type << '\n';
          for (const auto &r : oracle.at(phase))
            if (u(r[2]) == m.source.id) {
              const auto p = property(r);
              const auto &c = r[7];
              if (p != "GlobalPose" && p != "LinearVelocity" &&
                  p != "AngularVelocity" && p.find("LocalPose[") != 0)
                continue;
              const auto at = c == "qx" || c == "x"   ? 0
                              : c == "qy" || c == "y" ? 1
                              : c == "qz" || c == "z" ? 2
                              : c == "qw"             ? 3
                              : c == "px"             ? 4
                              : c == "py"             ? 5
                                                      : 6;
              expected << "f," << m.serial_id << ',' << m.owner_serial_id << ','
                       << m.shape_index << ',' << p << ',' << at << ',' << r[8]
                       << '\n';
            }
        }
        check(bool(expected), "SDK independent spatial expected output");
      };
      sdk_expected(output, exported, "origin_shift1_post");
      auto last = s.snapshot_at_event(end);
      auto reloaded = cropped.snapshot_at_event(crop.events.size());
      // Explicitly include the later-created rounding actor in both exports.
      ro.objects.clear();
      for (const auto &[oid, o] : last.instances)
        if (o.class_name == QualifiedName{"physx3", "PxRigidStatic"} ||
            o.class_name == QualifiedName{"physx3", "PxRigidDynamic"} ||
            o.class_name == QualifiedName{"physx3", "PxArticulation"})
          ro.objects.push_back({oid, o.generation});
      for (bool crop_output : {false, true}) {
        const auto prefix = output + (crop_output ? "_crop" : "_end");
        auto value = repx::export_snapshot(s.metadata(),
                                           crop_output ? reloaded : last, ro);
        std::ofstream file(std::filesystem::u8path(prefix + ".repx"),
                           std::ios::binary);
        file << value.xml;
        check(bool(file), "RepX end/crop output");
        sdk_expected(prefix, value, markers.back().name);
      }
    }
  }
  std::cout << "origin-shift SDK: " << checked
            << " exact float/getter checks; forward/reverse/checkpoints, "
               "save/crop, mixed queries and cameras passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 3 || argc == 4, "usage: origin_shift_test capture.pxd2 "
                                  "expected.csv [repx_output_prefix]");
    run(argv[1], argv[2], argc == 4 ? argv[3] : "");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
