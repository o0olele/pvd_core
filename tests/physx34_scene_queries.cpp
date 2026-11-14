#include "pvd/scene_queries.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
namespace {
using namespace pvd;
namespace r = pvd::render;
void need(bool v, const std::string &text) {
  if (!v)
    throw std::runtime_error(text);
}
void close(float a, float b, const std::string &text) {
  need(std::isfinite(a) && std::isfinite(b) &&
           std::abs(a - b) <= 3e-5f + std::abs(b) * 3e-6f,
       text + " " + std::to_string(a) + " != " + std::to_string(b));
}
using Key = std::pair<std::uint64_t, std::size_t>;
using Rows = std::vector<std::string>;
Rows split(const std::string &line) {
  std::stringstream in(line);
  Rows r;
  std::string s;
  while (std::getline(in, s, ','))
    r.push_back(s);
  return r;
}
r::SceneQueryKind kind(const std::string &value) {
  if (value == "raycast")
    return r::SceneQueryKind::raycast;
  if (value == "sweep")
    return r::SceneQueryKind::sweep;
  if (value == "overlap")
    return r::SceneQueryKind::overlap;
  throw std::runtime_error("bad oracle kind");
}
float component(Vec3 v, const std::string &field) {
  if (field.back() == 'x')
    return v.x;
  if (field.back() == 'y')
    return v.y;
  return v.z;
}
float component(Quat q, const std::string &field) {
  if (field.back() == 'x')
    return q.x;
  if (field.back() == 'y')
    return q.y;
  if (field.back() == 'z')
    return q.z;
  return q.w;
}
void check_query(const r::SceneQuery &q, const std::string &field,
                 std::size_t item, const std::string &value) {
  const auto expected_u = [&] { return std::stoull(value); };
  const auto expected_f = [&] { return std::stof(value); };
  auto u = [&](std::uint64_t actual) {
    need(actual == expected_u(), field + " integer differs");
  };
  auto f = [&](float actual) { close(actual, expected_f(), field); };
  if (field == "type")
    u(q.type);
  else if (field == "filter_flags")
    u(q.filter_flags);
  else if (field == "filter_data")
    u(q.filter_data.at(0).at(item));
  else if (field == "hit_count")
    u(q.hits.size());
  else if (field == "sdk_has_block") {
    if (q.filter_flags & (1u << 5))
      need(q.has_block == false, "NO_BLOCK proves no blocking hit");
    else
      need(
          !q.has_block.has_value(),
          "collector does not transmit hasBlock even when SDK oracle knows it");
  } else if (field == "sdk_touches" || field == "sdk_query_status") {
  } // These SDK facts are intentionally absent from PVD.
  else if (field == "distance")
    f(*q.max_distance);
  else if (field.rfind("origin.", 0) == 0)
    f(component(*q.origin, field));
  else if (field.rfind("direction.", 0) == 0)
    f(component(*q.unit_direction, field));
  else if (field.rfind("pose.", 0) == 0) {
    const auto p = q.overlap_pose ? *q.overlap_pose : q.poses.at(0);
    if (field.rfind("pose.p.", 0) == 0)
      f(component(p.position, field));
    else
      f(component(p.rotation, field));
  } else if (field.rfind("geometry.", 0) == 0) {
    const auto &g = q.geometries.at(0);
    if (field == "geometry.kind")
      u(g.value.index());
    else if (field.rfind("geometry.extents.", 0) == 0)
      f(component(std::get<r::Box>(g.value).half_extents, field));
    else if (field == "geometry.radius")
      f(std::visit(
          [](const auto &p) -> float {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, r::Sphere> ||
                          std::is_same_v<T, r::Capsule>)
              return p.radius;
            else
              throw std::runtime_error("oracle radius wrong kind");
          },
          g.value));
    else if (field == "geometry.half_height")
      f(std::get<r::Capsule>(g.value).half_height);
    else {
      const auto &c = std::get<r::SceneQueryConvex>(g.value);
      if (field == "geometry.resource") {
        u(c.resource_id);
        need(c.resource && c.resource->id == c.resource_id,
             "convex resource generation resolved");
      } else if (field == "geometry.flags")
        u(c.flags);
      else if (field.rfind("geometry.scale.", 0) == 0)
        f(component(c.scale.scale, field));
      else if (field.rfind("geometry.rotation.", 0) == 0)
        f(component(c.scale.rotation, field));
      else
        throw std::runtime_error("unknown geometry oracle field");
    }
  } else if (field.rfind("hit.", 0) == 0) {
    const auto &h = q.hits.at(item);
    if (field == "hit.shape") {
      u(h.shape_id);
      need(h.shape && h.shape->id == h.shape_id && h.shape->generation == 0,
           "hit shape exact ID/generation");
    } else if (field == "hit.actor") {
      u(h.actor_id);
      need(h.actor && h.actor->id == h.actor_id && h.actor->generation == 0,
           "hit actor exact ID/generation");
    } else if (field == "hit.face")
      u(h.face_index);
    else if (field == "hit.flags")
      u(h.flags);
    else if (field == "hit.distance") {
      need(h.distance_available, "SDK computed distance must be available");
      f(h.distance);
    } else if (field.rfind("hit.position.", 0) == 0) {
      need(h.position_available, "recorded hit position availability");
      f(component(h.position, field));
    } else if (field.rfind("hit.normal.", 0) == 0) {
      need(h.normal_available, "recorded hit normal availability");
      f(component(h.normal, field));
    } else if (field == "hit.u") {
      need(h.uv_available, "recorded UV availability");
      f(h.u);
    } else if (field == "hit.v") {
      need(h.uv_available, "recorded UV availability");
      f(h.v);
    } else
      throw std::runtime_error("unknown hit oracle field");
  } else
    throw std::runtime_error("unknown oracle field " + field);
}
void run(const std::string &path, const std::string &csv) {
  auto session = Session::load_file(path);
  r::SceneQueryIndex index;
  std::map<Key, r::SceneQueries> frames;
  std::map<std::uint64_t, std::size_t> local;
  std::size_t queries = 0, hits = 0, lines = 0;
  for (const auto &marker : session.frames()) {
    auto result = index.extract(session, marker);
    if (!result.complete) {
      for (const auto &d : result.diagnostics)
        std::cerr << d.property << " seq " << d.source.sequence << " offset "
                  << d.payload_offset << ": " << d.message << '\n';
    }
    need(result.complete,
         "real SDK query extraction must be structurally complete");
    const auto frame = local[marker.scene_id]++;
    need(result.scene.id == marker.scene_id && result.scene.generation == 0,
         "scene generation");
    for (const auto &q : result.queries) {
      need(q.complete &&
               q.source.publication_event_count > marker.begin_event_count &&
               q.source.publication_event_count <= marker.event_count,
           "fresh source prefix");
      need(!q.requested_hit_flags && !q.sweep_inflation,
           "never invent omitted SDK input flags/inflation");
      ++queries;
      hits += q.hits.size();
      for (const auto &h : q.hits) {
        need(h.role == ((q.filter_flags & (1u << 5))
                            ? r::SceneQueryHitRole::touching
                            : r::SceneQueryHitRole::unreported),
             "touch/block roles only when recorded evidence proves them");
        if (q.source.kind == r::SceneQueryKind::overlap)
          need(!h.position_available && !h.normal_available &&
                   !h.distance_available && !h.uv_available &&
                   !h.face_index_available,
               "overlap dummy zero fields are unavailable");
      }
      const auto snapshot =
          session.snapshot_at_event(q.source.snapshot_event_count);
      for (const auto &g : q.geometries) {
        const auto *object = snapshot.instance(g.key.id);
        need(object && object->generation == g.key.generation,
             "geometry source generation at publication");
      }
    }
    lines += result.lines.size();
    for (const auto &line : result.lines)
      need(line.derived && line.source.scene == result.scene,
           "display line source provenance");
    frames.emplace(Key{marker.scene_id, frame}, std::move(result));
  }
  need(frames.size() == 6 && local.size() == 2,
       "two scenes and three native frames each");
  std::ifstream input(std::filesystem::u8path(csv));
  need(bool(input), "open oracle CSV");
  std::string line;
  std::getline(input, line);
  std::size_t checked = 0;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const auto row = split(line);
    need(row.size() == 9, "oracle CSV shape");
    const auto frame = std::stoull(row[0]), scene = std::stoull(row[1]);
    const auto &result = frames.at({scene, frame});
    if (row[3] == "collection") {
      const bool collected = std::stoul(row[8]) != 0;
      for (const auto &c : result.collections)
        need(c.state == (collected ? r::SceneQueryCollectionState::recorded
                                   : r::SceneQueryCollectionState::uncollected),
             "collector availability must follow fresh native publications");
      if (frame == 2)
        need(result.queries.empty() && result.lines.empty(),
             "empty/disabled frame must not repeat old queries");
      continue;
    }
    const auto wanted_kind = kind(row[3]);
    const bool batched = std::stoul(row[2]) != 0;
    const auto sequence = std::stoull(row[4]);
    const r::SceneQuery *query = nullptr;
    for (const auto &q : result.queries)
      if (q.source.kind == wanted_kind && q.source.batched == batched &&
          q.source.sequence == sequence) {
        need(!query, "unique source sequence within native publication");
        query = &q;
      }
    need(query, "oracle query missing from capture");
    check_query(*query, row[5], std::stoull(row[6]), row[8]);
    ++checked;
  }
  std::cout << "coverage " << queries << " queries " << hits << " hits "
            << lines << " lines " << checked << " fields\n";
  need(input.eof() && checked > 2000 && queries == 80 && hits > 50 &&
           lines > 50,
       "actual oracle coverage, counts and output");
  // Reverse seeks use the retained publication index, never a future snapshot.
  for (auto it = session.frames().rbegin(); it != session.frames().rend();
       ++it) {
    auto reversed = index.extract(session, *it);
    need(reversed.complete, "reverse scene-query seek");
  }
  std::cout << "Scene query SDK oracle passed: " << queries << " queries, "
            << hits << " hits, " << lines << " derived lines, " << checked
            << " getter fields, 2 scenes / 6 native frames\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    need(argc == 3, "usage: physx34_scene_queries CAPTURE EXPECTED_CSV");
    run(argv[1], argv[2]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
