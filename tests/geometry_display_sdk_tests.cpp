// Pure C++ independent display oracle over recordings made by the real SDK.
// render_expected.csv is produced by examples/physx34_render_capture.cpp from
// SDK getter world vertices/normals/bounds, without pvd_core in the producer.
// No ImGui, Windows, D3D, or PhysX SDK linkage is required by this consumer.
// Optional argv[3] is the scene-query capture ("-" skips it); argv[4] is the
// six-joint capture. The runtime's separate synthetic/WARP tests cover UI/GPU.
#include "model.hpp"
#include "pvd/repx.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
using Row = std::vector<std::string>;
void need(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct D {
  double x, y, z;
};
D operator+(D a, D b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D operator-(D a, D b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D operator*(D a, double s) { return {a.x * s, a.y * s, a.z * s}; }
D from(Vec3 p) { return {p.x, p.y, p.z}; }
D from(const Row &row, std::size_t i) {
  return {std::stod(row.at(i)), std::stod(row.at(i + 1)),
          std::stod(row.at(i + 2))};
}
double norm(D p) { return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); }
Vec3 floats(D p) { return {float(p.x), float(p.y), float(p.z)}; }
void near(D a, D b, const char *what) {
  need(norm(a - b) < 2e-4 * (1 + norm(b)), what);
}
D independently_world(const Transform &pose, Vec3 p) {
  const auto q = pose.rotation;
  const double x = q.x, y = q.y, z = q.z, w = q.w;
  return {pose.position.x + (1 - 2 * y * y - 2 * z * z) * p.x +
              (2 * x * y - 2 * z * w) * p.y + (2 * x * z + 2 * y * w) * p.z,
          pose.position.y + (2 * x * y + 2 * z * w) * p.x +
              (1 - 2 * x * x - 2 * z * z) * p.y + (2 * y * z - 2 * x * w) * p.z,
          pose.position.z + (2 * x * z - 2 * y * w) * p.x +
              (2 * y * z + 2 * x * w) * p.y +
              (1 - 2 * x * x - 2 * y * y) * p.z};
}
std::map<unsigned, std::vector<Row>> rows(const char *path) {
  std::ifstream in(path);
  need(bool(in), "oracle CSV open");
  std::map<unsigned, std::vector<Row>> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.front() == '#')
      continue;
    std::istringstream s(line);
    Row r;
    std::string v;
    while (std::getline(s, v, ','))
      r.push_back(v);
    out[unsigned(std::stoul(r.at(1)))].push_back(std::move(r));
  }
  return out;
}
std::string wire_fingerprint(const Snapshot &snapshot) {
  std::ostringstream out;
  out << snapshot.event_count;
  for (const auto &item : snapshot.instances) {
    out << '|' << item.first << ':' << item.second.generation;
    for (const auto &field : item.second.properties) {
      out << field.first << ':' << field.second.incoming_type.str() << ':'
          << field.second.item_count << ':' << field.second.raw_bytes().size();
      out.write(reinterpret_cast<const char *>(field.second.raw_bytes().data()),
                std::streamsize(field.second.raw_bytes().size()));
      if (field.second.reference_ids)
        for (auto id : *field.second.reference_ids)
          out << 'r' << id;
    }
  }
  return out.str();
}
void inside(const OrbitCamera &camera, const render::Bounds &bounds) {
  const auto m = camera.view_projection(1.6).values;
  for (float x : {bounds.minimum.x, bounds.maximum.x})
    for (float y : {bounds.minimum.y, bounds.maximum.y})
      for (float z : {bounds.minimum.z, bounds.maximum.z}) {
        const double clip[] = {
            double(m[0]) * x + double(m[4]) * y + double(m[8]) * z + m[12],
            double(m[1]) * x + double(m[5]) * y + double(m[9]) * z + m[13],
            double(m[2]) * x + double(m[6]) * y + double(m[10]) * z + m[14],
            double(m[3]) * x + double(m[7]) * y + double(m[11]) * z + m[15]};
        need(clip[3] > 0 && std::abs(clip[0] / clip[3]) <= 1.0001 &&
                 std::abs(clip[1] / clip[3]) <= 1.0001 &&
                 clip[2] / clip[3] >= -1e-6 && clip[2] / clip[3] <= 1.00001,
             "displayed SDK bounds fit all 8 corners");
      }
}
void sdk(const char *capture, const char *expected) {
  Document doc;
  doc.open_file(capture);
  need(doc.set_sample_point(SamplePoint::after_fetch), "afterfetch mode");
  const auto data = rows(expected);
  need(doc.timeline().size() == 12, "12 SDK fixture samples");
  std::size_t points = 0, fits = 0, ray_hits = 0;
  for (std::size_t frame = 0; frame < 12; ++frame) {
    need(doc.set_frame(frame), "SDK sample seek");
    auto options = doc.view_options();
    options.geometry_scale = 1;
    doc.set_view_options(options);
    const auto original = doc.frame();
    const auto raw = wire_fingerprint(original->snapshot);
    repx::Options ex;
    ex.unsupported = repx::UnsupportedPolicy::allow_partial;
    const auto xml =
        repx::export_snapshot(*doc.metadata(), original->snapshot, ex).xml;
    for (float scale : {.125f, 1.f, 7.75f}) {
      options.geometry_scale = scale;
      doc.set_view_options(options);
      const auto current = doc.frame();
      need(wire_fingerprint(current->snapshot) == raw,
           "display option modified raw state");
      need(repx::export_snapshot(*doc.metadata(), current->snapshot, ex).xml ==
               xml,
           "display option changed RepX XML");
      for (const auto &row : data.at(unsigned(frame + 1))) {
        if (row[0] != "shape" || row[4] != "1" || row[5] == "plane")
          continue;
        const auto actor = std::stoull(row[2]), shape = std::stoull(row[3]);
        const DrawItem *draw = nullptr;
        for (const auto &item : current->draws)
          if (item.key.actor.id == actor && item.key.shape.id == shape)
            draw = &item;
        need(draw, "SDK attachment exists");
        const auto pivot = from(row, 6),
                   minimum = pivot + (from(row, 27) - pivot) * scale,
                   maximum = pivot + (from(row, 30) - pivot) * scale;
        const auto b = display_bounds(*draw);
        need(bool(b), "finite displayed bounds");
        near(from(b->minimum), minimum, "SDK oracle scaled min");
        near(from(b->maximum), maximum, "SDK oracle scaled max");
        need(doc.select(draw->key), "select SDK attachment");
        OrbitCamera camera;
        need(doc.fit_selected(camera, 1.6), "SDK attachment fit");
        inside(camera, *b);
        ++fits;
        for (const auto &vertex : data.at(unsigned(frame + 1))) {
          if (vertex[0] != "vertex" || std::stoull(vertex[2]) != actor ||
              std::stoull(vertex[3]) != shape)
            continue;
          const auto native = from(vertex, 8),
                     scaled = pivot + (native - pivot) * scale;
          bool found = false;
          for (auto local : draw->mesh->positions)
            if (norm(independently_world(draw->world_pose, local) - native) <
                1e-4) {
              near(from(display_point(*draw, local)), scaled,
                   "SDK vertex oracle display point");
              found = true;
              break;
            }
          if (found)
            ++points;
        }
        for (const auto &triangle : data.at(unsigned(frame + 1))) {
          if (triangle[0] != "triangle" || std::stoull(triangle[2]) != actor ||
              std::stoull(triangle[3]) != shape)
            continue;
          D center{};
          for (std::size_t corner = 0; corner < 3; ++corner) {
            bool got = false;
            for (const auto &vertex : data.at(unsigned(frame + 1)))
              if (vertex[0] == "vertex" && std::stoull(vertex[2]) == actor &&
                  std::stoull(vertex[3]) == shape &&
                  vertex[4] == triangle[5 + corner]) {
                center = center + from(vertex, 8) * (1. / 3);
                got = true;
                break;
              }
            need(got, "SDK triangle references getter vertex");
          }
          center = pivot + (center - pivot) * scale;
          const auto n = from(triangle, 8);
          const double offset = .001 * scale;
          const Ray ray{floats(center + n * offset), floats(n * -1)};
          auto isolated = *draw;
          isolated.face_culling = FaceCulling::back;
          const auto hit = pick({isolated}, ray);
          need(bool(hit), "scaled native SDK triangle front is CPU pickable");
          near(from(hit->position), center,
               "scaled CPU pick matches independent SDK world triangle "
               "centroid");
          ++ray_hits;
        }
      }
      need(doc.frame()->render.instances.size() ==
               original->render.instances.size(),
           "source frame count stable");
    }
    need(wire_fingerprint(original->snapshot) == raw,
         "retained COW frame changed");
  }
  need(points > 500 && fits > 300 && ray_hits > 500,
       "oracle exercised mesh points and all shape kinds");
  std::cout << "real SDK display oracle: " << points
            << " scaled resource vertices, " << fits << " fits, " << ray_hits
            << " native triangle ray hits; exact raw/RepX unchanged\n";
}
void queries(const char *capture) {
  Document doc;
  doc.open_file(capture);
  std::size_t published = 0;
  for (const auto &scene : doc.scenes()) {
    need(doc.set_scene(scene.key.id), "query scene");
    for (std::size_t i = 0; i < doc.timeline().size(); ++i) {
      need(doc.set_frame(i), "query frame");
      auto options = doc.view_options();
      options.geometry_scale = 1;
      doc.set_view_options(options);
      const auto before = doc.frame();
      const auto raw = wire_fingerprint(before->snapshot);
      options.geometry_scale = 19;
      doc.set_view_options(options);
      const auto after = doc.frame();
      need(before->query_volumes.size() == after->query_volumes.size() &&
               before->scene_queries.queries.size() ==
                   after->scene_queries.queries.size(),
           "queries unchanged by shape scale");
      for (std::size_t j = 0; j < before->query_volumes.size(); ++j) {
        const auto &a = before->query_volumes[j];
        const auto &b = after->query_volumes[j];
        need(a.mesh == b.mesh, "query immutable mesh identity unchanged");
        near(from(a.pose.position), from(b.pose.position),
             "query pose unchanged");
        ++published;
      }
      need(wire_fingerprint(after->snapshot) == raw,
           "query source raw changed");
    }
  }
  need(published > 0, "real SDK query volumes exercised");
  std::cout << "real SDK query display isolation: " << published
            << " native volumes unchanged\n";
}
void overflow(const char *capture) {
  Document original;
  original.open_file(capture);
  need(original.set_sample_point(SamplePoint::after_fetch),
       "overflow source sampling");
  need(original.set_frame(original.timeline().size() - 1),
       "overflow source last frame");
  const auto source = original.frame();
  const render::ShapeInstance *box = nullptr;
  for (const auto &shape : source->render.instances)
    if (render::geometry_kind(shape.geometry) == render::GeometryKind::box) {
      box = &shape;
      break;
    }
  need(box, "SDK box geometry exists");
  auto events = original.session()->events();
  events.resize(source->snapshot.event_count);
  constexpr std::uint64_t stream = 0xf00d;
  const auto emit = [&](Payload payload) {
    events.push_back({stream, 0, 0, std::move(payload)});
  };
  emit(StringHandleEvent{"physx3", 1});
  emit(StringHandleEvent{"PxVec3", 2});
  emit(StringHandleEvent{"HalfExtents", 3});
  emit(StringHandleEvent{"frame", 4});
  Bytes bytes;
  const float huge = 2e35f;
  std::uint32_t bits;
  std::memcpy(&bits, &huge, 4);
  for (int n = 0; n < 3; ++n)
    for (unsigned b = 0; b < 4; ++b)
      bytes.push_back(static_cast<std::uint8_t>(
          bits >>
          (8 * (original.session()->header().byte_order == ByteOrder::little
                    ? b
                    : 3 - b))));
  emit(SetPropertyValue{box->geometry_id, 3, {1, 2}, 1, bytes});
  emit(BeginSection{box->scene_id, 4, 100});
  emit(EndSection{box->scene_id, 4, 101});
  Document doc;
  doc.adopt(std::make_unique<Session>(original.session()->header(),
                                      std::move(events)));
  need(doc.set_frame(doc.timeline().size() - 1), "huge source frame");
  const auto before = doc.frame();
  need(!before->draws.empty(), "huge source shape tessellated");
  const auto raw = wire_fingerprint(before->snapshot);
  auto options = doc.view_options();
  options.geometry_scale = 10000;
  bool rejected = false;
  try {
    doc.set_view_options(options);
  } catch (const std::overflow_error &) {
    rejected = true;
  }
  need(rejected && doc.frame() == before &&
           doc.view_options().geometry_scale == 1 &&
           wire_fingerprint(doc.frame()->snapshot) == raw,
       "finite scale overflow must retain prior frame and view atomically");
  options.geometry_scale = 2;
  doc.set_view_options(options);
  need(doc.frame() != before && doc.view_options().geometry_scale == 2,
       "document remains usable after rejected overflow");
  std::cout << "real SDK-derived finite overflow: COW/view/raw strong rollback "
               "and later valid update passed\n";
  need(doc.set_frame(0), "return to small source frame");
  options.geometry_scale = 10000;
  doc.set_view_options(options);
  const auto small = doc.frame();
  const auto index = doc.frame_index();
  rejected = false;
  try {
    doc.set_frame(doc.timeline().size() - 1);
  } catch (const std::overflow_error &) {
    rejected = true;
  }
  std::cout << "seek overflow state: rejected=" << rejected
            << " document_index=" << doc.frame_index()
            << " retained_frame_index=" << doc.frame()->frame_index
            << " old_index=" << index
            << " old_frame_retained=" << (doc.frame() == small) << '\n';
  need(rejected && doc.frame() == small && doc.frame_index() == index,
       "failed scaled seek must retain consistent cursor and frame");
  Playback playing;
  playing.playing = true;
  playing.frames_per_second = 1000;
  doc.set_playback(playing);
  rejected = false;
  try {
    doc.advance(1);
  } catch (const std::overflow_error &) {
    rejected = true;
  }
  need(rejected && doc.frame() == small && doc.frame_index() == index &&
           doc.playback().playing,
       "failed scaled playback advance retains cursor/frame/playback");
  auto all = doc.session()->events();
  need(all.size() > 7, "live SDK-derived source size");
  std::vector<Event> prefix(all.begin(), all.end() - 7),
      tail(all.end() - 7, all.end());
  Document live;
  live.begin_live(doc.session()->header());
  live.append_live(std::move(prefix));
  auto live_options = live.view_options();
  live_options.geometry_scale = 10000;
  live.set_view_options(live_options);
  const auto published = live.frame();
  const auto published_index = live.frame_index();
  need(live.append_live(std::move(tail)),
       "display overflow publishes live diagnostic");
  need(live.frame_index() == published_index &&
           live.frame()->snapshot.event_count ==
               published->snapshot.event_count &&
           wire_fingerprint(live.frame()->snapshot) ==
               wire_fingerprint(published->snapshot) &&
           !live.follow_live() && live.live_event_count() == all.size() &&
           !live.frame()->errors.empty(),
       "accepted live prefix survives display overflow with paused historical "
       "sample");
  live.set_follow_live(true);
  need(
      !live.follow_live() && live.frame_index() == published_index,
      "unrepaired follow remains paused without rejecting valid source events");
  live_options.geometry_scale = 1;
  live.set_view_options(live_options);
  live.set_follow_live(true);
  need(live.follow_live() && live.frame_index() == live.timeline().size() - 1 &&
           live.frame()->snapshot.event_count == all.size(),
       "live remains recoverable after user reduces visual scale");
  std::cout << "scaled overflow navigation: seek/playback rollback, accepted "
               "live prefix, paused display and later recovery passed\n";
}
void joint_scales(const char *capture) {
  Document doc;
  doc.open_file(capture);
  std::size_t checked = 0;
  for (std::size_t i = 0; i < doc.timeline().size(); ++i) {
    need(doc.set_frame(i), "joint native frame");
    const auto original = doc.frame();
    const auto raw = wire_fingerprint(original->snapshot);
    std::size_t count = 0;
    for (const auto &c : original->debug.constraints)
      count += std::holds_alternative<render::DebugJointFrames>(c.payload);
    need(count > 0, "real SDK typed joint frame messages exist");
    for (float scale : {1.f, .125f, 7.75f}) {
      auto options = doc.view_options();
      options.geometry_scale = 2;
      options.joint_frame_scale = scale;
      options.joint_limit_scale = scale * .5f;
      doc.set_view_options(options);
      const auto frame = doc.frame();
      need(wire_fingerprint(frame->snapshot) == raw,
           "joint visual scales alter raw state");
      need(frame->joint_frame_lines.size() == count * 6,
           "six axes per typed joint frame");
      std::size_t line = 0;
      for (const auto &c : frame->debug.constraints)
        if (const auto *axes =
                std::get_if<render::DebugJointFrames>(&c.payload)) {
          for (const auto *transform : {&axes->parent, &axes->child})
            for (unsigned axis = 0; axis < 3; ++axis) {
              const float extent =
                  scale * (transform == &axes->parent ? 1.5f : 1.f);
              Vec3 local{};
              if (axis == 0)
                local.x = extent;
              else if (axis == 1)
                local.y = extent;
              else
                local.z = extent;
              const auto &actual = frame->joint_frame_lines.at(line++);
              near(from(actual.position0), from(transform->position),
                   "SDK frame axis origin remains native");
              near(from(actual.position1),
                   independently_world(*transform, local),
                   "joint frame display multiplier matches independent "
                   "quaternion matrix");
              ++checked;
            }
        }
      need(frame->joint_limit_lines.size() ==
               original->joint_limit_lines.size(),
           "display scale preserves limit topology");
    }
  }
  need(checked > 0, "joint SDK axes checked");
  std::cout
      << "real SDK joint display: " << checked
      << " independently transformed axis endpoints and raw isolation passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    need(
        argc >= 3 && argc <= 5,
        "usage: pvd_geometry_display_sdk_tests <render_capture.pxd2> "
        "<render_expected.csv> [queries_capture.pxd2|-] [joints_capture.pxd2]");
    sdk(argv[1], argv[2]);
    if (argc > 3 && std::string(argv[3]) != "-")
      queries(argv[3]);
    if (argc > 4)
      joint_scales(argv[4]);
    overflow(argv[1]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "SDK geometry display oracle failed: " << e.what() << '\n';
    return 1;
  }
}
