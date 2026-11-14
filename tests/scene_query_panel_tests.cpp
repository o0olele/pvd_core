#include "imgui.h"
#include "imgui_internal.h"
#include "renderer.hpp"
#include "scene_query_panel.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace pvd;
using namespace pvd::viewer;
namespace r = pvd::render;
namespace {
void check(bool v, const char *message) {
  if (!v)
    throw std::runtime_error(message);
}
constexpr std::uint64_t scene_a = 0xF000000000000101ULL,
                        scene_b = 0xF000000000000102ULL,
                        actor_id = 0xFEDCBA9876543210ULL,
                        shape_id = 0xFEDCBA9876543211ULL;
struct BytesWriter {
  Bytes data;
  ByteOrder order;
  explicit BytesWriter(ByteOrder v) : order(v) {}
  void u(std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
      data.push_back(static_cast<std::uint8_t>(
          value >> (8 * (order == ByteOrder::little ? i : size - i - 1))));
  }
  void f(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    u(bits, 4);
  }
  void text(const std::string &value) {
    data.insert(data.end(), value.begin(), value.end());
    data.push_back(0);
  }
  void zero(std::size_t end) { data.resize(end, 0); }
  void hit() {
    const auto begin = data.size();
    u(shape_id, 8);
    u(actor_id, 8);
    u(99, 4);
    u(1 | 2 | 8 | 1024, 4);
    for (float v : {0.f, 1.f, 2.f, 0.f, 1.f, 0.f, 3.f, .25f, .5f})
      f(v);
    zero(begin + 64);
  }
  void ray(std::size_t base, std::size_t hits, bool batch = false,
           bool bad = false) {
    const auto begin = data.size();
    u(0, 4);
    for (unsigned i = 0; i < 4; ++i)
      u(i + 1, 4);
    u(batch ? 32 : 3, 4);
    for (float v : {0.f, 1.f, 0.f, 1.f, 0.f, 0.f, 10.f})
      f(v);
    zero(begin + 56);
    u(0xFEFFFFFFFFFFFFFFULL, 8);
    u(base, 4);
    u(hits, 4);
    zero(begin + 72);
    text(bad     ? "Missing.Array"
         : batch ? "BatchedQueries.Hits"
                 : "SceneQueries.Hits");
  }
};
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  std::size_t first_prefix = 0;
  explicit Fixture(ByteOrder order = ByteOrder::little) {
    header.byte_order = order;
    header.stream_flags = 1;
    const auto scene = type("PxScene"), actor = type("PxActor"),
               shape = type("PxShape");
    emit(CreateClass{scene});
    emit(CreateClass{actor});
    emit(CreateClass{shape});
    emit(CreateClass{type("PvdRaycast")});
    emit(CreateClass{type("PvdSqHit")});
    for (const auto *prefix : {"SceneQueries.", "BatchedQueries."})
      for (const auto *name : {"Raycasts", "Hits"}) {
        auto p = text(std::string(prefix) + name);
        auto dt = type(std::string(name) == "Hits" ? "PvdSqHit" : "PvdRaycast");
        emit(CreateProperty{scene, p, 0, dt, 2, {}});
      }
    emit(CreateInstance{scene, scene_a});
    emit(CreateInstance{scene, scene_b});
    emit(CreateInstance{actor, actor_id});
    emit(CreateInstance{shape, shape_id});
    mark(true, scene_a, "frame");
    BytesWriter hits(order);
    hits.hit();
    set(scene_a, "SceneQueries.Hits", "PvdSqHit", 1, hits.data);
    BytesWriter rays(order);
    rays.ray(0, 1);
    for (int i = 1; i < 300; ++i)
      rays.ray(1, 0);
    set(scene_a, "SceneQueries.Raycasts", "PvdRaycast", 300, rays.data);
    set(scene_a, "BatchedQueries.Hits", "PvdSqHit", 1, hits.data);
    BytesWriter batch(order);
    batch.ray(0, 1, true);
    set(scene_a, "BatchedQueries.Raycasts", "PvdRaycast", 1, batch.data);
    mark(false, scene_a, "frame");
    first_prefix = events.size();
    mark(true, scene_a, "validation_after_fetch");
    mark(false, scene_a, "validation_after_fetch");
    mark(true, scene_b, "frame");
    set(scene_b, "SceneQueries.Hits", "PvdSqHit", 0, {});
    BytesWriter second(order);
    second.ray(0, 0);
    set(scene_b, "SceneQueries.Raycasts", "PvdRaycast", 1, second.data);
    mark(false, scene_b, "frame");
    mark(true, scene_a, "frame");
    BytesWriter bad(order);
    bad.ray(0, 1, false, true);
    set(scene_a, "SceneQueries.Raycasts", "PvdRaycast", 1, bad.data);
    mark(false, scene_a, "frame");
    mark(true, scene_a, "frame");
    emit(DestroyInstance{actor_id});
    emit(CreateInstance{actor, actor_id});
    mark(false, scene_a, "frame");
  }
  void emit(Payload p) {
    events.push_back({0xF100000000000001ULL, events.size() * 5,
                      events.size() * 19, std::move(p)});
  }
  std::uint32_t text(const std::string &value) {
    const auto found = handles.find(value);
    if (found != handles.end())
      return found->second;
    const auto id = static_cast<std::uint32_t>(handles.size() + 1);
    handles[value] = id;
    emit(StringHandleEvent{value, id});
    return id;
  }
  WireName type(const std::string &value) {
    auto ns = text("physx3"), n = text(value);
    return {ns, n};
  }
  void set(std::uint64_t id, const char *property, const char *datatype,
           std::uint32_t count, Bytes bytes) {
    auto p = text(property);
    auto dt = type(datatype);
    emit(SetPropertyValue{id, p, dt, count, std::move(bytes)});
  }
  void mark(bool begin, std::uint64_t id, const char *name) {
    auto h = text(name);
    auto t = events.size() * 5;
    if (begin)
      emit(BeginSection{id, h, t});
    else
      emit(EndSection{id, h, t});
  }
  void load(Document &doc) const {
    doc.adopt(std::make_unique<Session>(header, events),
              "synthetic recorded query ABI");
    check(doc.set_scene(scene_a), "synthetic scene absent");
    check(doc.set_frame(0), "synthetic frame absent");
  }
};
struct Output {
  std::string text;
  float overflow = 0;
};
Output draw(Document &doc, OrbitCamera &camera, RenderOptions &options,
            SceneQueryPanelState &state, float width = 1060,
            std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
  auto &io = ImGui::GetIO();
  io.DisplaySize = {width, 2000};
  io.DeltaTime = 1.f / 60;
  if (mouse)
    io.AddMousePosEvent(mouse->x, mouse->y);
  if (down)
    io.AddMouseButtonEvent(0, *down);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({width, 1900});
  ImGui::Begin("Scene query panel test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::LogToBuffer();
  drawSceneQueries(doc, camera, options, state);
  Output result;
  result.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  auto *window = ImGui::GetCurrentWindow();
  result.overflow =
      std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
  ImGui::End();
  ImGui::Render();
  check(ImGui::GetDrawData()->TotalVtxCount > 0, "real ImGui draw data absent");
  return result;
}
void click(Document &doc, OrbitCamera &camera, RenderOptions &options,
           SceneQueryPanelState &state, float width, ImVec2 at) {
  draw(doc, camera, options, state, width, at, false);
  draw(doc, camera, options, state, width, at, true);
  draw(doc, camera, options, state, width, at, false);
}
void widgets(float width, ByteOrder order) {
  Fixture fixture(order);
  Document doc;
  fixture.load(doc);
  OrbitCamera camera;
  RenderOptions options;
  SceneQueryPanelState state;
  auto first = draw(doc, camera, options, state, width);
  draw(doc, camera, options, state, width);
  if (doc.frame()->scene_queries.queries.size() != 301 ||
      state.row_count != 301) {
    std::cerr << "queries=" << doc.frame()->scene_queries.queries.size()
              << " rows=" << state.row_count
              << " frame=" << doc.frame()->snapshot.event_count << "\n";
    for (const auto &d : doc.frame()->scene_queries.diagnostics)
      std::cerr << d.property << ": " << d.message << "\n";
  }
  check(doc.frame()->scene_queries.queries.size() == 301 &&
            state.row_count == 301,
        "query transport not read from actual Session");
  check(state.visible_row_count > 0 && state.visible_row_count < 30,
        "query list not virtualized");
  check(first.text.find("uncollected") != std::string::npos &&
            first.text.find("Empty hits do not prove a miss") !=
                std::string::npos,
        "transport availability/empty outcome explanation missing");
  check(first.overflow <= 1, "query panel overflowed narrow viewport");
  const auto row = state.visible_rows.front();
  click(doc, camera, options, state, width,
        {row.left + 20, row.top + row.height * .5f});
  check(doc.selected_query() &&
            same_query_source(*doc.selected_query(), row.source) &&
            doc.frame()->snapshot.event_count == fixture.first_prefix &&
            doc.selected_query()->snapshot_event_count < fixture.first_prefix &&
            doc.selected_query()->sequence == 0,
        "real query click lost exact source prefix");
  state.selected_hit = 0;
  const auto details = draw(doc, camera, options, state, width);
  check(state.hit_row_count == 1 &&
            details.text.find("0xFEDCBA9876543210") != std::string::npos &&
            details.text.find("unreported role") != std::string::npos,
        "raw 64-bit IDs or unavailable hit role lost");
  check(details.text.find("Requested hit flags: not transmitted") !=
                std::string::npos &&
            details.text.find("hasBlock: not transmitted") != std::string::npos,
        "untransmitted status fabricated");
  check(details.overflow <= 1, "query detail overflowed narrow viewport");
  check(doc.fit_query(camera, width / 800.0) && std::isfinite(camera.distance),
        "query fit failed on finite requested path");
  auto wrong = *doc.selected_query();
  ++wrong.publication_event_count;
  check(!doc.select_query(wrong),
        "query selection accepted wrong publication identity");
  state.batched_filter = true;
  auto batched = draw(doc, camera, options, state, width);
  check(state.row_count == 1, "batch filter mixed native calls");
  const auto batchrow = state.visible_rows.front();
  click(doc, camera, options, state, width,
        {batchrow.left + 20, batchrow.top + batchrow.height * .5f});
  state.selected_hit = 0;
  const auto batch_details = draw(doc, camera, options, state, width);
  check(doc.selected_query()->batched &&
            batch_details.text.find("touching") != std::string::npos &&
            batch_details.text.find("false (derived from eNO_BLOCK)") !=
                std::string::npos,
        "NO_BLOCK derived facts mislabeled");
  state.batched_filter.reset();
  state.kind_filter = r::SceneQueryKind::sweep;
  draw(doc, camera, options, state, width);
  check(state.row_count == 0, "kind filter ignored");
  state.kind_filter.reset();
  const std::string id_filter = "fedcba9876543210";
  std::copy(id_filter.begin(), id_filter.end(), state.text_filter.begin());
  draw(doc, camera, options, state, width);
  check(state.row_count == 2, "raw 64-bit hit ID filter truncated or ignored");
  state.text_filter[0] = 0;
  check(doc.set_frame(1), "malformed frame absent");
  auto malformed = draw(doc, camera, options, state, width);
  check(!doc.selected_query() && !state.selected_hit &&
            !doc.frame()->scene_queries.complete &&
            malformed.text.find("malformed") != std::string::npos,
        "seek kept stale selection or concealed malformed transport");
  check(doc.set_frame(2), "uncollected frame absent");
  auto absent = draw(doc, camera, options, state, width);
  check(state.row_count == 0 && doc.frame()->scene_queries.queries.empty() &&
            absent.text.find("uncollected") != std::string::npos,
        "old arrays repeated as fresh data");
  check(doc.frame()->snapshot.instance(actor_id)->generation == 1,
        "synthetic object reuse not represented");
  check(doc.set_scene(scene_b), "second scene absent");
  draw(doc, camera, options, state, width);
  check(state.row_count == 1 && doc.frame()->scene_queries.scene.id == scene_b,
        "scene switch mixed source scenes");
  const auto emptyrow = state.visible_rows.front();
  click(doc, camera, options, state, width,
        {emptyrow.left + 10, emptyrow.top + emptyrow.height * .5f});
  const auto empty = draw(doc, camera, options, state, width);
  check(state.hit_row_count == 0 &&
            empty.text.find("not a proven miss") != std::string::npos,
        "empty transmitted range called a miss");
  check(doc.set_scene(scene_a) &&
            doc.set_sample_point(SamplePoint::after_fetch),
        "after-fetch marker absent");
  const auto after = draw(doc, camera, options, state, width);
  check(state.row_count == 0 &&
            after.text.find("Cached arrays are not fresh") !=
                std::string::npos &&
            after.text.find("Use Frame end") != std::string::npos,
        "after-fetch marker rendered stale arrays");
  Document replacement;
  fixture.load(replacement);
  state.error = "stale error";
  draw(replacement, camera, options, state, width);
  check(state.error.empty() && state.row_count == 301 && !state.selected_hit,
        "source replacement leaked panel state");
  state.reset();
  check(!state.cached_source && state.row_count == 0 &&
            state.visible_rows.empty(),
        "panel reset retained old query data");
}
void sdk(const char *path) {
  Document doc;
  doc.open_file(path);
  OrbitCamera camera;
  RenderOptions options;
  SceneQueryPanelState state;
  std::size_t queries = 0, hits = 0, windows = 0, recorded_empty = 0,
              uncollected = 0;
  std::vector<std::uint64_t> scene_ids;
  for (const auto &scene : doc.scenes())
    scene_ids.push_back(scene.key.id);
  for (const auto scene : scene_ids) {
    doc.set_scene(scene);
    doc.set_sample_point(SamplePoint::native_frame);
    const auto count = doc.timeline().size();
    for (std::size_t i = 0; i < count; ++i) {
      doc.set_frame(i);
      draw(doc, camera, options, state, 420);
      const auto &data = doc.frame()->scene_queries;
      ++windows;
      queries += data.queries.size();
      for (const auto &q : data.queries)
        hits += q.hits.size();
      if (data.queries.empty()) {
        if (std::any_of(data.collections.begin(), data.collections.end(),
                        [](const auto &c) {
                          return c.state ==
                                     r::SceneQueryCollectionState::recorded &&
                                 c.query_count == 0;
                        }))
          ++recorded_empty;
        if (std::all_of(data.collections.begin(), data.collections.end(),
                        [](const auto &c) {
                          return c.state ==
                                 r::SceneQueryCollectionState::uncollected;
                        }))
          ++uncollected;
      }
    }
  }
  check(windows == 6 && queries == 80 && hits == 96,
        "real SDK query fixture window/query/hit counts changed");
  check(
      recorded_empty == 1 && uncollected == 1,
      "real SDK recorded-empty and disabled/uncollected windows were confused");
  std::cout << "real SDK query panel: " << queries << " queries, " << hits
            << " hit records\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char *pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    widgets(1060, ByteOrder::little);
    widgets(420, ByteOrder::little);
    widgets(420, ByteOrder::big);
    if (argc > 1)
      sdk(argv[1]);
    ImGui::DestroyContext();
    std::cout << "scene query panel tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "scene query panel tests failed: " << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
