#include "scene_query_fixture.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
using namespace scene_query_test;
namespace r = pvd::render;
void check(bool condition, const char *text) {
  if (!condition)
    throw std::runtime_error(text);
}
void close(float a, float b, const char *text) {
  check(std::isfinite(a) && std::abs(a - b) < 2e-5f, text);
}
void close(Vec3 a, Vec3 b, const char *text) {
  close(a.x, b.x, text);
  close(a.y, b.y, text);
  close(a.z, b.z, text);
}
bool diagnostic(const r::SceneQueries &out, r::SceneQueryCode code) {
  for (const auto &d : out.diagnostics)
    if (d.code == code)
      return true;
  return false;
}
r::SceneQueries ray_case(Fixture &f, Bytes ray, std::uint32_t count = 1,
                         r::SceneQueryOptions options = {}) {
  const auto begin = f.events.size();
  f.auxiliaries();
  f.set(f.scene, "SceneQueries.Raycasts", "PvdRaycast", std::move(ray), count);
  auto s = f.session();
  return r::extract_scene_queries(
      s, r::SceneQueryWindow{f.scene, begin, s.event_count()}, options);
}
void abi(ByteOrder order, std::uint8_t pointers) {
  Fixture f(order, pointers);
  const auto begin = f.events.size();
  f.publish();
  f.publish(true);
  auto s = f.session();
  r::SceneQueryIndex index;
  const auto result =
      index.extract(s, r::SceneQueryWindow{f.scene, begin, s.event_count()});
  if (!result.complete)
    for (const auto &d : result.diagnostics)
      std::cerr << d.property << ": " << d.message << '\n';
  check(result.complete && result.diagnostics.empty() &&
            result.queries.size() == 8 && result.lines.size() == 10,
        "all query native layouts in both byte orders and pointer widths");
  for (const auto &collection : result.collections)
    check(collection.state == r::SceneQueryCollectionState::recorded &&
              collection.publication_event_count > begin,
          "all six freshly published families");
  for (const auto &q : result.queries) {
    check(q.complete && q.source.scene.id == f.scene &&
              q.source.scene.generation == 0 && q.source.stream_id == 17 &&
              q.source.publication_event_count > begin &&
              q.source.snapshot_event_count == q.source.publication_event_count,
          "scene/source/prefix identity");
    check(q.hits.size() == 1 && q.hits[0].actor_id == f.actor &&
              q.hits[0].shape_id == f.shape && q.hits[0].actor &&
              q.hits[0].actor->generation == 0,
          "raw source-ABI pointer values and object generations");
    check(!q.requested_hit_flags && !q.sweep_inflation,
          "unrecorded requested inputs remain unavailable");
    if (q.source.kind == r::SceneQueryKind::raycast) {
      check(q.filter_data[0][0] == 0xf1234501u &&
                q.filter_data[0][3] == 0xf1234504u,
            "unsigned filter words");
      close(*q.origin, {-3, 2, 3}, "ray origin");
      close(*q.unit_direction, {1, 0, 0}, "ray direction");
      close(*q.max_distance, 9, "requested distance");
      if (q.source.sequence == 0) {
        const auto &h = q.hits[0];
        check(h.position_available && h.normal_available &&
                  h.distance_available && h.uv_available &&
                  h.face_index_available && h.face_index == 0xf1234567u,
              "returned validity bits preserved");
        close(h.position, {1, 2, 3},
              "recorded impact is not synthesized from distance");
        close(h.u, .25f, "UV");
        check(!q.has_block && h.role == r::SceneQueryHitRole::unreported,
              "closest wire type does not invent unrecorded hasBlock");
      } else {
        const auto &h = q.hits[0];
        check(q.has_block == false &&
                  h.role == r::SceneQueryHitRole::touching &&
                  !h.position_available && !h.normal_available &&
                  !h.uv_available && h.distance_available,
              "NO_BLOCK and unavailable NaN placeholders");
        check(std::isnan(h.position.x) && std::isnan(h.normal.z),
              "unavailable raw bits retained");
      }
    } else {
      check(q.geometries.size() == 1 && q.geometries[0].key.id == f.box &&
                q.geometries[0].key.generation == 0,
            "query geometry real instance key");
      close(std::get<r::Box>(q.geometries[0].value).half_extents, {1, 2, 3},
            "query box extents");
      if (q.source.kind == r::SceneQueryKind::sweep) {
        check(q.poses.size() == 1 && q.filter_data.size() == 1 &&
                  q.pose_range.array_name ==
                      f.prefix(q.source.batched) + "PoseList",
              "sweep ranges resolve independently");
        close(q.poses[0].position, {2, 3, 4}, "sweep pose");
        check(!q.hits[0].uv_available, "UV does not become valid on a sweep");
      } else {
        check(q.overlap_pose && q.poses.size() == 1 &&
                  !q.hits[0].position_available &&
                  !q.hits[0].normal_available && !q.hits[0].distance_available,
              "overlap dummy zero fields never become an impact");
        close(q.poses[0].position, {10, 20, 30}, "overlap inline pose");
      }
    }
  }
  for (const auto &line : result.lines) {
    check(line.derived && line.source.scene.id == f.scene,
          "display line provenance");
    if (line.kind == r::SceneQueryLineKind::impact_normal) {
      check(line.hit_index == 0, "normal has its original hit index");
      close(line.position0, {1, 2, 3}, "normal starts at recorded impact");
      close(line.position1, {1, 2.5f, 3}, "display normal length");
    }
  }
  auto isolated = index.extract(
      s, r::SceneQueryWindow{f.other_scene, begin, s.event_count()});
  check(isolated.complete && isolated.queries.empty() && isolated.lines.empty(),
        "another scene never receives these query arrays");
}
void freshness_and_reverse() {
  Fixture f;
  const auto begin_a = f.events.size();
  f.publish();
  const auto end_a = f.events.size();
  const auto begin_b = end_a;
  f.publish();
  const auto end_b = f.events.size();
  f.emit(ErrorMessage{0, "unrelated", "", 0});
  const auto end_c = f.events.size();
  auto session = f.session();
  r::SceneQueryIndex index;
  index.update(session);
  const auto a = index.extract(session,
                               r::SceneQueryWindow{f.scene, begin_a, end_a}),
             b = index.extract(session,
                               r::SceneQueryWindow{f.scene, begin_b, end_b}),
             c = index.extract(session,
                               r::SceneQueryWindow{f.scene, end_b, end_c});
  check(a.complete && b.complete && a.queries.size() == 4 &&
            b.queries.size() == 4,
        "identical raw publications are fresh in every native frame");
  check(a.queries[0].source.publication_event_count <
            b.queries[0].source.publication_event_count,
        "publication source prefixes remain distinct");
  const auto state_a = session.snapshot_at_event(end_a),
             state_b = session.snapshot_at_event(end_b);
  check(state_a.instance(f.scene)->property("SceneQueries.Raycasts")->bytes ==
            state_b.instance(f.scene)->property("SceneQueries.Raycasts")->bytes,
        "exercise byte-identical assignments omitted from Session state-diff "
        "replay index");
  check(c.complete && c.queries.empty() && c.lines.empty(),
        "no current publication must not repeat cached arrays");
  for (const auto &family : c.collections)
    check(family.state == r::SceneQueryCollectionState::uncollected,
          "explicit uncollected state");
  auto again =
      index.extract(session, r::SceneQueryWindow{f.scene, begin_a, end_a});
  check(again.queries[0].source.publication_event_count ==
            a.queries[0].source.publication_event_count,
        "reverse seek excludes future publications");
  auto combined =
      index.extract(session, r::SceneQueryWindow{f.scene, begin_a, end_b});
  check(!combined.complete &&
            diagnostic(combined, r::SceneQueryCode::multiple_publications) &&
            combined.queries.size() == 4 &&
            combined.queries[0].source.publication_event_count ==
                b.queries[0].source.publication_event_count,
        "multiple publication window is explicit and does not stack history");
  FrameMarker marker;
  marker.scene_id = f.scene;
  marker.begin_event_count = begin_a;
  marker.event_count = end_a;
  check(index.extract(session, marker).queries.size() == 4,
        "FrameMarker convenience path");
}
void incremental_and_streams() {
  Fixture f;
  const auto begin = f.events.size();
  f.auxiliaries();
  const auto property = f.str("SceneQueries.Raycasts");
  const auto type = f.type("PvdRaycast");
  f.emit(BeginSetPropertyValue{f.scene, property, type});
  f.emit(AppendPropertyValueData{f.ray(), 1});
  const auto partial = f.events.size();
  f.emit(EndSetPropertyValue{});
  const auto end = f.events.size();
  Session live(f.header);
  live.append(std::vector<Event>(f.events.begin(),
                                 f.events.begin() +
                                     static_cast<std::ptrdiff_t>(partial)));
  r::SceneQueryIndex index;
  auto pending =
      index.extract(live, r::SceneQueryWindow{f.scene, begin, partial});
  check(pending.complete && pending.queries.empty(),
        "partial opaque property does not publish a query");
  live.append(std::vector<Event>(
      f.events.begin() + static_cast<std::ptrdiff_t>(partial), f.events.end()));
  auto complete = index.extract(live, r::SceneQueryWindow{f.scene, begin, end});
  check(complete.complete && complete.queries.size() == 1 &&
            index.indexed_event_count() == end,
        "incremental chunk commit publishes once");
  live.finish();
  Fixture split;
  const auto split_begin = split.events.size();
  split.auxiliaries();
  const auto split_property = split.str("SceneQueries.Raycasts");
  const auto split_type = split.type("PvdRaycast");
  split.emit(BeginSetPropertyValue{split.scene, split_property, split_type});
  for (int chunk = 0; chunk < 2; ++chunk) {
    Bytes data;
    for (int i = 0; i < 32; ++i) {
      auto record = split.ray();
      data.insert(data.end(), record.begin(), record.end());
    }
    split.emit(AppendPropertyValueData{std::move(data), 32});
  }
  split.emit(EndSetPropertyValue{});
  auto split_session = split.session();
  auto split_result = r::extract_scene_queries(
      split_session, r::SceneQueryWindow{split.scene, split_begin,
                                         split_session.event_count()});
  check(split_result.complete && split_result.queries.size() == 64 &&
            split_result.queries.back().source.sequence == 63,
        "native String fixups and variable item offsets survive multiple "
        "32-item SDK-style chunks");
  Fixture multi;
  multi.stream = 29;
  const auto begin_multi = multi.events.size();
  multi.publish();
  auto stream_session = multi.session();
  auto q = r::extract_scene_queries(
      stream_session, r::SceneQueryWindow{multi.scene, begin_multi,
                                          stream_session.event_count()});
  check(q.complete && q.queries.size() == 4 &&
            q.queries[0].source.stream_id == 29,
        "string-handle namespaces remain per stream");
  bool threw = false;
  try {
    index.update(stream_session);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  check(threw, "index cannot silently bind a different Session");
  index.reset();
  index.update(stream_session);
  check(index.indexed_event_count() == stream_session.event_count(),
        "explicit reset rebinding");
}
void object_generations_and_dangling() {
  Fixture f;
  const auto begin = f.events.size();
  f.auxiliaries();
  f.set(f.scene, "SceneQueries.Raycasts", "PvdRaycast", f.ray(), 1);
  const auto publication = f.events.size();
  f.emit(DestroyInstance{f.actor});
  f.emit(CreateInstance{f.type("PxActor"), f.actor});
  f.actor_scene(f.scene);
  const auto end = f.events.size();
  auto session = f.session();
  auto result = r::extract_scene_queries(
      session, r::SceneQueryWindow{f.scene, begin, end});
  check(
      result.complete && result.queries[0].hits[0].actor->generation == 0 &&
          session.latest().instance(f.actor)->generation == 1 &&
          result.queries[0].source.snapshot_event_count == publication,
      "resolve hit generation at publication, not a later same-ID incarnation");
  Fixture missing;
  const auto mb = missing.events.size();
  missing.auxiliaries(false, missing.hit(7, 999), 1);
  missing.set(missing.scene, "SceneQueries.Raycasts", "PvdRaycast",
              missing.ray(), 1);
  const auto me = missing.events.size();
  missing.emit(CreateInstance{missing.type("PxActor"), 999});
  missing.set(
      999, "Scene", "VoidPtr",
      [&] {
        BytesWriter w(missing.header.byte_order);
        w.u(missing.scene, 8);
        return w.bytes;
      }(),
      1);
  auto ms = missing.session();
  r::SceneQueryIndex index;
  index.update(ms);
  auto no_future =
      index.extract(ms, r::SceneQueryWindow{missing.scene, mb, me});
  check(!no_future.complete &&
            diagnostic(no_future, r::SceneQueryCode::dangling_reference) &&
            no_future.queries.size() == 1 &&
            no_future.queries[0].hits.size() == 1 &&
            no_future.queries[0].hits[0].actor_id == 999 &&
            !no_future.queries[0].hits[0].actor && no_future.lines.empty(),
        "preserve unresolved raw ID without future-object leakage or "
        "fabricated geometry");
  Fixture foreign;
  foreign.actor_scene(foreign.other_scene);
  auto cross = ray_case(foreign, foreign.ray());
  check(!cross.complete && diagnostic(cross, r::SceneQueryCode::malformed) &&
            cross.queries[0].hits[0].actor_id == foreign.actor &&
            cross.lines.empty(),
        "known foreign-scene hit is diagnostic, never drawn");
  Fixture reuse;
  const auto first_begin = reuse.events.size();
  reuse.publish();
  const auto first_end = reuse.events.size();
  reuse.emit(DestroyInstance{reuse.scene});
  reuse.emit(CreateInstance{reuse.type("PxScene"), reuse.scene});
  reuse.geometry_present[0] = false;
  const auto second_begin = reuse.events.size();
  reuse.publish();
  auto rs = reuse.session();
  r::SceneQueryIndex ri;
  ri.update(rs);
  auto newest = ri.extract(rs, r::SceneQueryWindow{reuse.scene, second_begin,
                                                   rs.event_count()}),
       oldest = ri.extract(
           rs, r::SceneQueryWindow{reuse.scene, first_begin, first_end});
  check(newest.complete && oldest.complete && newest.scene.generation == 1 &&
            oldest.scene.generation == 0,
        "source scene generations remain isolated across reuse and reverse "
        "seeks");
}
void malformed() {
  for (auto order : {ByteOrder::little, ByteOrder::big})
    for (auto width : {std::uint8_t{4}, std::uint8_t{8}}) {
      Fixture seed(order, width);
      const auto bytes = seed.ray();
      for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        Fixture f(order, width);
        auto result = ray_case(
            f, Bytes(bytes.begin(),
                     bytes.begin() + static_cast<std::ptrdiff_t>(cut)));
        check(!result.complete && result.queries.empty(),
              "every native query/tail truncation fails without partial "
              "collection commit");
      }
    }
  {
    Fixture f;
    auto bytes = f.ray();
    bytes.push_back(9);
    auto r = ray_case(f, bytes);
    check(!r.complete && r.queries.empty() &&
              diagnostic(r, r::SceneQueryCode::malformed),
          "trailing opaque bytes rejected");
  }
  {
    Fixture f;
    auto r = ray_case(f, f.ray(), 2);
    check(!r.complete && r.queries.empty(),
          "item count cannot outlive native structures");
  }
  {
    Fixture f;
    auto r = ray_case(f, f.ray(), UINT32_MAX);
    check(!r.complete && diagnostic(r, r::SceneQueryCode::limit_exceeded) &&
              r.queries.empty(),
          "hostile array count rejected before allocation");
  }
  {
    Fixture f;
    auto r = ray_case(f, f.ray(false, 1, UINT32_MAX, 2));
    check(!r.complete && r.queries.size() == 1 && r.queries[0].hits.empty() &&
              diagnostic(r, r::SceneQueryCode::out_of_range),
          "unsigned array range cannot overflow");
  }
  {
    Fixture f;
    auto r = ray_case(f, f.ray(false, 1, 0, 1, 3, {-3, 2, 3}, {1, 0, 0}, 9,
                               "BatchedQueries.Hits"));
    check(!r.complete && diagnostic(r, r::SceneQueryCode::malformed),
          "foreign collector array names rejected");
  }
  {
    Fixture f;
    const auto begin = f.events.size();
    f.auxiliaries();
    f.set(f.scene, "SceneQueries.Raycasts", "PvdSweep", f.ray(), 1);
    auto s = f.session();
    auto r = r::extract_scene_queries(
        s, r::SceneQueryWindow{f.scene, begin, s.event_count()});
    check(!r.complete && diagnostic(r, r::SceneQueryCode::invalid_type),
          "unknown opaque type is never guessed");
  }
  {
    Fixture f;
    auto r = ray_case(f, f.ray(false, 9));
    check(!r.complete && diagnostic(r, r::SceneQueryCode::unsupported_query),
          "query type must match family");
  }
  for (int bad = 0; bad < 3; ++bad) {
    Fixture f;
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    auto r = ray_case(f, f.ray(false, 1, 0, 1, 3,
                               bad == 0 ? Vec3{nan, 0, 0} : Vec3{},
                               bad == 1 ? Vec3{2, 0, 0} : Vec3{1, 0, 0},
                               bad == 2 ? -1.f : 9.f));
    check(!r.complete && r.lines.empty(),
          "invalid recorded inputs do not emit paths");
  }
  {
    Fixture f;
    const auto begin = f.events.size();
    auto hit =
        f.hit(1, 0, 0, 2, {std::numeric_limits<float>::quiet_NaN(), 0, 0});
    f.auxiliaries(false, hit, 1);
    f.set(f.scene, "SceneQueries.Raycasts", "PvdRaycast", f.ray(), 1);
    auto s = f.session();
    auto r = r::extract_scene_queries(
        s, r::SceneQueryWindow{f.scene, begin, s.event_count()});
    check(!r.complete && r.queries[0].hits.empty(),
          "nonfinite available impact rejects whole hit staging");
  }
  {
    Fixture f;
    const auto begin = f.events.size();
    f.auxiliaries();
    f.set(f.scene, "SceneQueries.Sweeps", "PvdSweep", f.sweep(), 1);
    auto s = f.session();
    auto result = r::extract_scene_queries(
        s, r::SceneQueryWindow{f.scene, begin + 5, s.event_count()});
    check(!result.complete &&
              diagnostic(result, r::SceneQueryCode::stale_array),
          "old auxiliary data cannot masquerade as this window's hit array");
  }
  {
    Fixture f;
    const auto begin = f.events.size();
    f.auxiliaries(false, f.hit(0, 0, 0, -.75f), 1);
    f.set(f.scene, "SceneQueries.Sweeps", "PvdSweep", f.sweep(), 1);
    auto s = f.session();
    auto result = r::extract_scene_queries(
        s, r::SceneQueryWindow{f.scene, begin, s.event_count()});
    check(result.complete && result.queries[0].hits[0].distance_available &&
              result.queries[0].hits[0].distance == -.75f,
          "negative sweep penetration distance is retained, not clamped");
  }
}
void limits() {
  for (int limit = 0; limit < 7; ++limit) {
    Fixture f;
    const auto begin = f.events.size();
    f.publish();
    auto s = f.session();
    r::SceneQueryOptions o;
    switch (limit) {
    case 0:
      o.max_queries = 1;
      break;
    case 1:
      o.max_hits = 1;
      break;
    case 2:
      o.max_aux_items = 0;
      break;
    case 3:
      o.max_array_bytes = 10;
      break;
    case 4:
      o.max_reference_string_bytes = 4;
      break;
    case 5:
      o.max_lines = 1;
      break;
    case 6:
      o.max_index_entries = 3;
      break;
    }
    auto result = r::extract_scene_queries(
        s, r::SceneQueryWindow{f.scene, begin, s.event_count()}, o);
    check(!result.complete &&
              diagnostic(result, r::SceneQueryCode::limit_exceeded),
          "each independent parser/output/index budget is explicit");
    check(result.queries.size() <= o.max_queries &&
              result.lines.size() <= o.max_lines,
          "output respects hard limits");
    std::size_t hits = 0;
    for (const auto &q : result.queries)
      hits += q.hits.size();
    check(hits <= o.max_hits,
          "partial query failures cannot bypass total hit budget");
  }
  Fixture f;
  const auto begin = f.events.size();
  f.publish();
  auto s = f.session();
  r::SceneQueryOptions aux_limit;
  aux_limit.max_aux_items = 3;
  auto bounded = r::extract_scene_queries(
      s, r::SceneQueryWindow{f.scene, begin, s.event_count()}, aux_limit);
  std::size_t aux_count = 0;
  for (const auto &q : bounded.queries)
    aux_count += q.geometries.size() + q.poses.size() + q.filter_data.size();
  check(!bounded.complete && aux_count <= 3,
        "shared small auxiliary ranges cannot amplify output past the global "
        "budget");
  r::SceneQueryOptions o;
  o.max_diagnostics = 1;
  o.max_hits = 0;
  auto r = r::extract_scene_queries(
      s, r::SceneQueryWindow{f.scene, begin, s.event_count()}, o);
  check(!r.complete && r.diagnostics.size() == 1,
        "diagnostic cap preserves incomplete state");
  o = {};
  o.emit_paths = false;
  o.emit_normals = false;
  r = r::extract_scene_queries(
      s, r::SceneQueryWindow{f.scene, begin, s.event_count()}, o);
  check(r.complete && r.lines.empty() && r.queries.size() == 4,
        "geometry output is optional");
  r = r::extract_scene_queries(
      s, r::SceneQueryWindow{f.scene, s.event_count() + 1, s.event_count()});
  check(!r.complete && diagnostic(r, r::SceneQueryCode::invalid_range),
        "invalid query window");
  r = r::extract_scene_queries(
      s, r::SceneQueryWindow{999, begin, s.event_count()});
  check(!r.complete && diagnostic(r, r::SceneQueryCode::missing_scene),
        "missing scene is explicit");
  for (int invalid = 0; invalid < 3; ++invalid) {
    o = {};
    if (invalid == 0)
      o.normal_length = -1;
    if (invalid == 1)
      o.max_diagnostics = 0;
    if (invalid == 2)
      o.normal_length = std::numeric_limits<float>::quiet_NaN();
    bool threw = false;
    try {
      r::SceneQueryIndex bad(o);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    check(threw, "invalid options rejected");
  }
}
void chunk_completion_after_cross_stream_scene_reuse() {
  for (auto order : {ByteOrder::little, ByteOrder::big})
    for (auto pointers : {std::uint8_t{4}, std::uint8_t{8}}) {
      Fixture f(order, pointers);
      const auto begin = f.events.size();
      const auto property = f.str("SceneQueries.Raycasts");
      const auto type = f.type("PvdRaycast");
      f.emit(BeginSetPropertyValue{f.scene, property, type});
      f.emit(AppendPropertyValueData{f.ray(), 1});
      f.stream = 29;
      f.emit(DestroyInstance{f.scene});
      f.emit(CreateInstance{f.type("PxScene"), f.scene});
      f.set(f.scene, "SceneQueries.Hits", "PvdSqHit", f.hit(), 1);
      f.stream = 17;
      f.emit(EndSetPropertyValue{});
      auto session = f.session();
      r::SceneQueryIndex index;
      const auto result = index.extract(session,
          r::SceneQueryWindow{f.scene, begin, session.event_count()});
      check(result.complete && result.scene.generation == 1 && result.queries.size() == 1 &&
                result.queries[0].source.scene.generation == 1,
            "chunk completion was attributed to a destroyed Begin generation");
    }
}
} // namespace
int main() {
  try {
    for (auto order : {ByteOrder::little, ByteOrder::big})
      for (auto pointers : {std::uint8_t{4}, std::uint8_t{8}})
        abi(order, pointers);
    freshness_and_reverse();
    incremental_and_streams();
    object_generations_and_dangling();
    malformed();
    limits();
    chunk_completion_after_cross_stream_scene_reuse();
    std::cout << "scene query tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
