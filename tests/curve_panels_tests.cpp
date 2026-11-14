// Actual Dear ImGui widgets and real Session snapshots, without Win32/GPU.
#include "curve_panels.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace pvd;
using namespace pvd::viewer;
namespace a = pvd::analysis;
namespace {
void check(bool v, const std::string &m) {
  if (!v)
    throw std::runtime_error(m);
}
Bytes u64(std::uint64_t n) {
  Bytes b;
  for (int i = 0; i < 8; ++i)
    b.push_back(static_cast<std::uint8_t>(n >> (i * 8)));
  return b;
}
Bytes pose(float y) {
  const float p[] = {0, 0, 0, 1, 0, y, 0};
  Bytes b(sizeof p);
  std::memcpy(b.data(), p, sizeof p);
  return b;
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  void emit(Payload p) {
    events.push_back({1, events.size(), events.size() * 4, std::move(p)});
  }
  std::uint32_t str(const std::string &s) {
    auto it = strings.find(s);
    if (it != strings.end())
      return it->second;
    auto h = static_cast<std::uint32_t>(strings.size() + 1);
    strings[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *s) {
    auto ns = str("physx3"), n = str(s);
    return {ns, n};
  }
  void mark(bool begin, const char *name, std::uint64_t tick,
            std::uint64_t scene = 1) {
    auto h = str(name);
    if (begin)
      emit(BeginSection{scene, h, tick});
    else
      emit(EndSection{scene, h, tick});
  }
  Fixture() {
    header.stream_flags = 1;
    header.timestamp_numerator = 1000;
    header.timestamp_denominator = 1;
    emit(CreateClass{type("PxScene")});
    emit(CreateClass{type("Node")});
    auto cls = type("Node"), u = type("PvdU64"), t = type("PxTransform"),
         ref = type("ObjectRef"), pointer = type("VoidPtr");
    auto count = str("counter"), p = str("GlobalPose"), r = str("reference");
    emit(CreateProperty{cls, count, 0, u, 1, {}});
    emit(CreateProperty{cls, p, 0, t, 1, {}});
    emit(CreateProperty{cls, r, 0, ref, 1, {}});
    emit(CreateInstance{type("PxScene"), 1});
    emit(CreateInstance{type("PxScene"), 2});
    emit(CreateInstance{cls, 100});
    for (unsigned i = 0; i < 6; ++i) {
      mark(true, "frame", i * 100);
      if (i == 3)
        emit(DestroyInstance{100});
      if (i == 4)
        emit(CreateInstance{cls, 100});
      if (i != 3) {
        emit(SetPropertyValue{100, r, pointer, 1, u64(1)});
        if (i)
          emit(
              SetPropertyValue{100, count, u, 1,
                               u64(i == 1 ? 9007199254740993ull : UINT64_MAX)});
        emit(SetPropertyValue{100, p, t, 1, pose(static_cast<float>(i))});
      }
      mark(false, "frame", i * 100 + 10);
      mark(true, "validation_after_fetch", i * 100 + 11);
      if (i != 3)
        emit(SetPropertyValue{100, p, t, 1, pose(100.f + i)});
      mark(false, "validation_after_fetch", i * 100 + 20);
      if (i == 2) {
        mark(true, "frame", 280, 2);
        mark(false, "frame", 290, 2);
      }
    }
  }
  void load(Document &doc) {
    doc.adopt(std::make_unique<Session>(header, events), "curve test");
    doc.set_scene(1);
    doc.set_frame(0);
  }
};
struct Output {
  std::string text, csv;
  float overflow = 0;
};
Output draw(CurvePanels &panels, Document &doc,
            CurvePage page = CurvePage::curves, float width = 1060,
            std::optional<ImVec2> pos = {}, std::optional<bool> down = {},
            float wheel = 0) {
  auto &io = ImGui::GetIO();
  io.DisplaySize = {width, 1000};
  io.DeltaTime = 1.f / 60;
  if (pos)
    io.AddMousePosEvent(pos->x, pos->y);
  if (down)
    io.AddMouseButtonEvent(0, *down);
  if (wheel)
    io.AddMouseWheelEvent(0, wheel);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({width, 900});
  ImGui::Begin("Curve panel test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::LogToBuffer();
  Output out;
  panels.draw(
      doc, page,
      [&](const char *, const std::function<void(std::ostream &)> &write) {
        std::ostringstream s;
        write(s);
        out.csv = s.str();
      });
  out.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  auto *w = ImGui::GetCurrentWindow();
  out.overflow = std::max(0.f, w->DC.CursorMaxPos.x - w->InnerRect.Max.x);
  ImGui::End();
  ImGui::Render();
  check(ImGui::GetDrawData()->TotalVtxCount > 0,
        "panel produced no actual ImGui geometry");
  return out;
}
void click(CurvePanels &p, Document &d, ImVec2 at, float width = 1060) {
  draw(p, d, CurvePage::curves, width, at, false);
  draw(p, d, CurvePage::curves, width, at, true);
  draw(p, d, CurvePage::curves, width, at, false);
}
void synthetic(float width) {
  Fixture f;
  Document doc;
  f.load(doc);
  CurvePanels panels;
  auto empty = draw(panels, doc, CurvePage::curves, width);
  check(empty.text.find("Select an object") != std::string::npos &&
            panels.series_count() == 0,
        "empty panel fabricated samples instead of selection guidance");
  check(doc.inspect(100), "fixture inspected object missing");
  const auto id = panels.add_inspected(doc, "GlobalPose", "position.y");
  auto *data = panels.series(id);
  check(data && data->samples.size() == 6,
        "scene timeline source was not respected");
  check(data->samples[1].value && a::plot_value(*data->samples[1].value) == 1,
        "native GlobalPose sample incorrectly used after-fetch state");
  check(data->samples[3].gap == a::Gap::object_absent &&
            data->samples[4].gap == a::Gap::generation_mismatch,
        "lifetime gaps were merged across reused ID");
  a::CurveRequest rq;
  rq.object = {100, 0};
  rq.property = "counter";
  panels.add_series(doc, rq);
  data = panels.series(0);
  check(panels.series(1)->samples[0].gap == a::Gap::property_missing,
        "uncaptured numeric property did not produce a gap");
  std::ostringstream csv;
  a::write_curve_csv(csv, *panels.series(1));
  check(csv.str().find("9007199254740993") != std::string::npos &&
            csv.str().find("18446744073709551615") != std::string::npos,
        "CSV lost 64-bit integer precision");
  panels.set_series_visible(1, false);
  draw(panels, doc, CurvePage::curves, width);
  auto out = draw(panels, doc, CurvePage::curves, width);
  check(panels.row_count() == 12 && panels.visible_row_count() == 3,
        "sample or visible numeric counts are wrong");
  const auto view = panels.plot_view();
  check(view.valid && view.x_min >= 1, "frame plot must start at one or above");
  const auto &st = ImGui::GetStyle();
  ImVec2 export_button{
      st.WindowPadding.x + ImGui::CalcTextSize("Refresh").x +
          2 * st.FramePadding.x + st.ItemSpacing.x + ImGui::GetFrameHeight() +
          st.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto (1 Hz)").x +
          st.ItemSpacing.x + 8,
      st.WindowPadding.y + ImGui::GetTextLineHeightWithSpacing() +
          ImGui::GetFrameHeight() * .5f};
  draw(panels, doc, CurvePage::curves, width, export_button, false);
  draw(panels, doc, CurvePage::curves, width, export_button, true);
  const auto exported =
      draw(panels, doc, CurvePage::curves, width, export_button, false);
  check(exported.csv.find("9007199254740993") != std::string::npos &&
            exported.csv.find("18446744073709551615") != std::string::npos,
        "actual Export CSV widget lost selected series or integer precision");
  const double x = 2, y = 1;
  ImVec2 point{
      view.left +
          static_cast<float>((x - view.x_min) / (view.x_max - view.x_min)) *
              view.width,
      view.top + view.height -
          static_cast<float>((y - view.y_min) / (view.y_max - view.y_min)) *
              view.height};
  click(panels, doc, point, width);
  check(doc.frame_index() == 1 && doc.frame()->snapshot.event_count ==
                                      data->samples[1].source.event_count,
        "actual mouse click did not seek original event prefix");
  check(doc.inspected_object() && doc.inspected_object()->id == 100,
        "plot click did not preserve object inspection");
  const auto clicked = draw(panels, doc, CurvePage::curves, width);
  check(clicked.text.find("Selected frame 2 | event prefix") !=
            std::string::npos,
        "selected sample label did not convert to one-based frame numbering");
  auto before = panels.plot_view();
  draw(panels, doc, CurvePage::curves, width, point, {}, 1);
  draw(panels, doc, CurvePage::curves, width);
  auto after = panels.plot_view();
  check(after.x_max - after.x_min < before.x_max - before.x_min,
        "wheel did not zoom real plot");
  panels.set_time_axis(true);
  draw(panels, doc, CurvePage::curves, width);
  check(panels.plot_view().time_axis && panels.plot_view().x_max < .001,
        "clock-calibrated time axis wrong");
  doc.set_sample_point(SamplePoint::after_fetch);
  draw(panels, doc, CurvePage::curves, width);
  check(a::plot_value(*panels.series(0)->samples[1].value) == 101,
        "source mode change did not refresh explicit after-fetch samples");
  check(panels.seek_sample(doc, 0, 2) && doc.frame_index() == 2 &&
            doc.sample_point() == SamplePoint::after_fetch,
        "scripted seek differs from source mode");
  panels.set_diff_frames(0, 2);
  panels.refresh_diff(doc);
  const auto *diff = panels.diff_result();
  check(diff && diff->before_event_count == doc.timeline()[0].event_count &&
            diff->after_event_count == doc.timeline()[2].event_count,
        "diff compared wrong mode or event prefixes");
  check(std::any_of(diff->objects.begin(), diff->objects.end(),
                    [](const auto &c) {
                      return c.object.id == 100 &&
                             c.kind == a::ChangeKind::changed &&
                             !c.properties.empty();
                    }),
        "real property changes absent from diff");
  auto diffout = draw(panels, doc, CurvePage::diff, width);
  check(diffout.text.find("Result A 1 -> B 3") != std::string::npos &&
            diffout.text.find("Changed") != std::string::npos &&
            panels.visible_row_count(CurvePage::diff) > 0,
        "diff widgets did not show actual object changes");
  panels.set_diff_frames(2, 4);
  panels.refresh_diff(doc);
  int old = 0, reused = 0;
  for (const auto &c : panels.diff_result()->objects)
    if (c.object.id == 100) {
      old += c.object.generation == 0 && c.kind == a::ChangeKind::destroyed;
      reused += c.object.generation == 1 && c.kind == a::ChangeKind::created;
    }
  check(old == 1 && reused == 1, "diff UI merged reused pointer generations");
  doc.set_scene(2);
  draw(panels, doc, CurvePage::curves, width);
  check(panels.series(0)->samples.size() == 1 &&
            panels.series(0)->samples[0].source.scene_id == 2,
        "scene filtering retained another scene timeline");
  check(out.overflow <= 1 && diffout.overflow <= 1,
        "panel controls overflow available width");
  check(panels.erase_series(0) && panels.series_count() == 1,
        "curve remove command failed");
  panels.reset();
  check(panels.series_count() == 0 && !panels.diff_result(),
        "reset retained old capture results");
  std::cout << "curve widgets width=" << width
            << " rows=12 mouse-prefix/lifetime/mode/diff passed\n";
}
void live_and_budget() {
  Fixture f;
  Document live;
  live.begin_live(f.header);
  CurvePanels p;
  const auto end =
      std::find_if(f.events.begin(), f.events.end(), [](const Event &e) {
        auto *v = std::get_if<EndSection>(&e.payload);
        return v && v->timestamp == 10;
      });
  const auto split = static_cast<std::size_t>(end - f.events.begin() + 1);
  live.append_live(std::vector<Event>(
      f.events.begin(), f.events.begin() + static_cast<std::ptrdiff_t>(split)));
  live.inspect(100);
  p.add_inspected(live, "GlobalPose", "position.y");
  check(p.series(0)->samples.size() == 1, "live first prefix curve missing");
  live.append_live(std::vector<Event>(
      f.events.begin() + static_cast<std::ptrdiff_t>(split), f.events.end()));
  draw(p, live);
  check(p.series(0)->samples.size() == 1,
        "normal UI frame resampled live history without refresh");
  p.refresh(live);
  check(p.series(0)->samples.size() == 6,
        "manual refresh did not extend live history");
  // A pending property chunk must not replace the already sampled prefix.
  const auto saved = p.series(0)->source_event_count;
  live.append_live(
      {Event{1, 0, 0,
             BeginSetPropertyValue{100, f.strings.at("GlobalPose"),
                                   WireName{f.strings.at("physx3"),
                                            f.strings.at("PxTransform")}}}});
  bool rejected = false;
  try {
    p.refresh(live);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  check(rejected && p.series(0)->source_event_count == saved,
        "pending live transaction replaced accepted curve prefix");
  live.append_live({Event{1, 0, 0, AppendPropertyValueData{pose(5), 1}},
                    Event{1, 0, 0, EndSetPropertyValue{}}});
  p.refresh(live);
  live.finish_live();
  Document doc;
  f.load(doc);
  CurvePanels skip;
  a::CurveRequest r;
  r.object = {100, 0};
  r.property = "GlobalPose";
  r.component = "position.y";
  r.stride = 2;
  skip.add_series(doc, r);
  a::CurveRequest ref;
  ref.object = {100, 0};
  ref.property = "reference";
  const auto ri = skip.add_series(doc, ref);
  check(skip.series(ri)->samples[0].gap == a::Gap::not_numeric &&
            !skip.series(ri)->samples[0].value,
        "reference was plotted as an integer");
  check(skip.series(0)->samples.size() == 3 &&
            skip.series(0)->samples[1].break_before,
        "skipped source rows were not marked as discontinuities");
  // More than the default budget verifies bounded output through the actual UI
  // sampler.
  auto events = f.events;
  auto frame = f.strings.at("frame");
  for (unsigned i = 0; i < 2100; ++i) {
    events.push_back({1, i, 0, BeginSection{1, frame, 1000 + i * 2}});
    events.push_back({1, i, 0, EndSection{1, frame, 1001 + i * 2}});
  }
  Document large;
  large.adopt(std::make_unique<Session>(f.header, std::move(events)));
  large.set_scene(1);
  large.set_frame(0);
  large.inspect(100);
  CurvePanels capped;
  capped.add_inspected(large, "GlobalPose", "position.y");
  check(capped.series(0)->resampled && capped.series(0)->samples.size() == 2000,
        "default UI budget exceeded 2000 samples");
}
void real(const char *file) {
  Document doc;
  doc.open_file(file);
  check(!doc.timeline().empty(), "real fixture has no frames");
  doc.set_frame(0);
  const InstanceState *actor = nullptr;
  for (const auto &it : doc.frame()->snapshot.instances)
    if (it.second.property("GlobalPose")) {
      actor = &it.second;
      break;
    }
  check(actor && doc.inspect(actor->id), "real SDK GlobalPose actor missing");
  CurvePanels p;
  p.add_inspected(doc, "GlobalPose", "position.y");
  check(p.series(0)->samples.size() == doc.timeline().size() &&
            p.series(0)->samples[0].value,
        "real SDK native curve missing");
  if (doc.set_sample_point(SamplePoint::after_fetch)) {
    draw(p, doc);
    check(p.series(0)->request.source.sampling == a::Sampling::after_fetch,
          "real explicit sampling mode ignored");
  }
  check(p.seek_sample(doc, 0, p.series(0)->samples.size() - 1),
        "real SDK last sample seek failed");
  p.set_diff_frames(0, doc.timeline().size() - 1);
  p.refresh_diff(doc);
  draw(p, doc, CurvePage::diff);
  check(p.error().empty(), "real SDK panels surfaced an error: " + p.error());
  std::cout << "real curve panel " << p.row_count() << " rows; "
            << p.row_count(CurvePage::diff) << " diff objects\n";
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
    synthetic(1060);
    synthetic(640);
    synthetic(420);
    live_and_budget();
    if (argc > 1)
      real(argv[1]);
    ImGui::DestroyContext();
    std::cout << "curve panel tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
