#include "imgui.h"
#include "imgui_internal.h"
#include "profile_curve_panel.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
using namespace pvd;
using namespace pvd::viewer;
namespace a = pvd::analysis;
namespace {
void check(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
void near(double x, double y, const char *m) {
  check(std::isfinite(x) && std::abs(x - y) < 1e-8, m);
}
template <class F> void rejects(F &&f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, "expected invalid profile curve request rejection");
}
struct Wire {
  Bytes bytes;
  ByteOrder order;
  explicit Wire(ByteOrder o = ByteOrder::little) : order(o) {}
  void u(std::uint64_t n, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          n >> ((order == ByteOrder::little ? i : width - 1 - i) * 8)));
  }
  void event(unsigned tag, std::uint64_t tick, std::uint16_t id = 1,
             std::uint32_t thread = 7, std::uint64_t context = 777) {
    u(tag, 1);
    u(15, 1);
    u(id, 2);
    u(thread, 4);
    u(context, 8);
    u(0, 1);
    u(0, 1);
    u(tick, 8);
  }
  void span(std::uint64_t begin, std::uint64_t end, std::uint16_t id = 1,
            std::uint32_t thread = 7) {
    event(1, begin, id, thread);
    event(2, end, id, thread);
  }
};
struct Fixture {
  StreamHeader header;
  std::uint64_t stream = 1;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  std::size_t late_prefix = 0;
  void emit(Payload p) {
    events.push_back({stream, events.size(), events.size() * 7, std::move(p)});
  }
  std::uint32_t text(const std::string &s) {
    auto f = handles.find(s);
    if (f != handles.end())
      return f->second;
    auto h = static_cast<std::uint32_t>(handles.size() + 1);
    handles[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *ns, const char *n) {
    auto a = text(ns), b = text(n);
    return {a, b};
  }
  void announce() {
    emit(AddProfileZone{10, "Curve, \"zone\""});
    emit(CreateInstance{type("_debugger_", "PxProfileZone"), 10});
    emit(AddProfileZoneEvent{10, "Work", 1, true});
    emit(AddProfileZoneEvent{10, "Cross-frame", 2, true});
    emit(AddProfileZoneEvent{10, "Never runs", 3, true});
    emit(AddProfileZoneEvent{10, "Disabled", 4, false});
  }
  void packet(Bytes b) {
    auto name = text("events");
    auto dt = type("physx3", "PvdU8");
    emit(SetPropertyValue{10, name, dt, static_cast<std::uint32_t>(b.size()),
                          std::move(b)});
  }
  void mark(bool begin, std::uint64_t tick, std::uint64_t scene = 1) {
    auto h = text("frame");
    if (begin)
      emit(BeginSection{scene, h, tick});
    else
      emit(EndSection{scene, h, tick});
  }
  void frame(std::uint64_t begin, std::uint64_t end, std::uint64_t scene = 1) {
    mark(true, begin, scene);
    mark(false, end, scene);
  }
  explicit Fixture(ByteOrder order = ByteOrder::little, bool populate = true) {
    header.byte_order = order;
    header.timestamp_numerator = 10;
    header.timestamp_denominator = 2;
    auto cls = type("_debugger_", "PxProfileZone");
    emit(CreateClass{cls});
    auto n = text("events"), semantic = text("profile event stream");
    auto dt = type("physx3", "PvdU8");
    emit(CreateProperty{cls, n, semantic, dt, 2, {}});
    announce();
    text("frame");
    if (!populate)
      return;
    frame(100, 200);
    Wire start(order);
    start.event(1, 100, 2);
    packet(start.bytes);
    frame(150, 250, 2);
    frame(200, 300);
    frame(300, 400);
    frame(400, 500);
    late_prefix = events.size();
    Wire late(order);
    late.event(2, 450, 2);
    late.span(110, 150);
    late.span(150, 250, 2);
    late.span(200, 200);
    late.span(210, 250, 1, 8);
    late.span(205, 255);
    late.span(480, 500);
    packet(late.bytes);
  }
  Session session() { return Session(header, events); }
};

a::ProfileCurveRequest
query(a::ProfileMetric metric = a::ProfileMetric::total_ticks) {
  a::ProfileCurveRequest q;
  q.filter.zone = profile::ZoneKey{10, 0};
  q.metric = metric;
  return q;
}
void load(Document &doc, const Fixture &f) {
  doc.adopt(std::make_unique<Session>(f.header, f.events));
  doc.set_scene(1);
  doc.set_frame(0);
}
struct Output {
  std::string text, csv;
  float overflow = 0, content_width = 0;
};
Output draw(ProfileCurvePanel &panel, Document &doc, float width = 1060,
            std::optional<ImVec2> mouse = {}, std::optional<bool> down = {},
            float wheel = 0) {
  auto &io = ImGui::GetIO();
  io.DisplaySize = {width, 1300};
  io.DeltaTime = 1.f / 60;
  if (mouse)
    io.AddMousePosEvent(mouse->x, mouse->y);
  if (down)
    io.AddMouseButtonEvent(0, *down);
  if (wheel)
    io.AddMouseWheelEvent(0, wheel);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({width, 1200});
  ImGui::Begin("Profile curve panel test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::LogToBuffer();
  Output result;
  panel.draw(doc, [&](const char *,
                      const std::function<void(std::ostream &)> &writer) {
    std::ostringstream out;
    writer(out);
    result.csv = out.str();
  });
  result.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  auto *window = ImGui::GetCurrentWindow();
  result.overflow =
      std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
  result.content_width = ImGui::GetContentRegionAvail().x;
  ImGui::End();
  ImGui::Render();
  check(ImGui::GetDrawData()->TotalVtxCount > 0,
        "actual ImGui panel draw data missing");
  return result;
}
Output click(ProfileCurvePanel &panel, Document &doc, float width, ImVec2 at) {
  draw(panel, doc, width, at, false);
  draw(panel, doc, width, at, true);
  return draw(panel, doc, width, at, false);
}
ImVec2 screen_point(const ProfileCurvePlotView &view, double x, double y) {
  return {view.left +
              static_cast<float>((x - view.x_min) / (view.x_max - view.x_min)) *
                  view.width,
          view.top + view.height -
              static_cast<float>((y - view.y_min) / (view.y_max - view.y_min)) *
                  view.height};
}
void widgets(float width) {
  Fixture f;
  const auto after = f.text("validation_after_fetch");
  f.emit(BeginSection{1, after, 600});
  f.emit(EndSection{1, after, 610});
  Document doc;
  load(doc, f);
  ProfileCurvePanel panel;
  auto empty = draw(panel, doc, width);
  check(panel.series_count() == 0 &&
            empty.text.find("No sample data is fabricated") !=
                std::string::npos,
        "empty UI fabricated a curve");
  panel.add_series(doc, query());
  check(panel.profile_index() && panel.profile_index()->zones().count({10, 0}),
        "actual advertised zone missing from persistent index");
  const auto initial_prefix = panel.indexed_event_count(),
             packets = panel.profile_index()->statistics().profile_packets;
  auto data = panel.series(0);
  check(data && data->samples.size() == 4 &&
            data->samples[0].total_ticks == "40" &&
            data->samples[1].total_ticks == "190" &&
            data->samples[3].total_ticks == "350",
        "UI used receipt frame instead of native end counter");
  check(!data->samples[2].value &&
            data->samples[2].state == a::ProfileSampleState::not_collected,
        "UI filled an unknown collection gap");
  draw(panel, doc, width);
  const auto first = draw(panel, doc, width);
  check(panel.row_count() == 4 && panel.visible_row_count() == 3 &&
            panel.plot_view().x_min >= 1 && panel.plot_view().unit == "ticks",
        "plot rows, gaps, or one-based native frame coordinates wrong");
  const auto at = screen_point(panel.plot_view(), 2, 190);
  click(panel, doc, width, at);
  check(doc.frame_index() == 1 &&
            doc.sample_point() == SamplePoint::native_frame &&
            doc.frame()->snapshot.event_count ==
                panel.series(0)->samples[1].source.event_count,
        "real mouse click did not seek exact native source frame/prefix");
  auto clicked = draw(panel, doc, width);
  check(clicked.text.find("Selected native frame 2") != std::string::npos &&
            clicked.text.find("190") != std::string::npos,
        "clicked exact value/frame label missing");
  check(doc.set_sample_point(SamplePoint::after_fetch),
        "test explicit after-fetch marker missing");
  const auto after_fetch = draw(panel, doc, width);
  check(panel.series(0)->samples.size() == 4 &&
            after_fetch.text.find("after-fetch data") != std::string::npos,
        "panel confused after-fetch sections with native timing windows");
  check(panel.seek_sample(doc, 0, 1) &&
            doc.sample_point() == SamplePoint::native_frame &&
            doc.frame_index() == 1,
        "sample seek did not switch after-fetch view back to native frame");
  draw(panel, doc, width);
  const auto old_view = panel.plot_view();
  draw(panel, doc, width, at, {}, 1);
  draw(panel, doc, width);
  check(panel.plot_view().x_max - panel.plot_view().x_min <
            old_view.x_max - old_view.x_min,
        "real wheel zoom did not change native frame view");
  // Real drag pans the frame range, while never exposing negative frame
  // numbers.
  auto view = panel.plot_view();
  ImVec2 start{view.left + view.width * .6f, view.top + view.height * .5f},
      end{start.x - 25, start.y};
  draw(panel, doc, width, start, false);
  draw(panel, doc, width, start, true);
  draw(panel, doc, width, end, true);
  draw(panel, doc, width, end, false);
  draw(panel, doc, width);
  check(panel.plot_view().x_min >= 1 && panel.plot_view().x_min > view.x_min,
        "plot drag did not pan bounded frame coordinates");
  panel.set_time_axis(true);
  draw(panel, doc, width);
  check(panel.plot_view().time_axis && panel.plot_view().x_max < .00001,
        "elapsed native clock time axis wrong");
  panel.set_time_axis(false);
  panel.add_series(doc, query(a::ProfileMetric::count));
  draw(panel, doc, width);
  check(panel.plot_view().unit == "count" && panel.visible_row_count() == 3,
        "count and ticks were plotted on a shared unit scale");
  panel.select_series(0);
  auto filtered = query();
  filtered.filter.event_id = 1;
  filtered.filter.thread_id = 8;
  filtered.filter.context_id = 777;
  panel.add_series(doc, filtered);
  check(panel.series(2)->samples[1].total_ticks == "40" &&
            panel.series(2)->matched_intervals == 1,
        "actual event/thread/context selector was ignored");
  panel.set_series_visible(2, false);
  panel.select_series(0);
  draw(panel, doc, width);
  check(panel.row_count() == 12 && panel.visible_row_count() == 3,
        "visibility/unit-group count mismatch");
  auto covered = query(a::ProfileMetric::mean_ticks);
  covered.coverage = a::ProfileCoverage::assume_continuous;
  panel.set_request(doc, 0, covered);
  draw(panel, doc, width);
  check(panel.request(0)->coverage == a::ProfileCoverage::assume_continuous &&
            panel.series(0)->samples[1].exact_value == "190/4" &&
            !panel.series(0)->samples[2].value,
        "editable coverage/mean policy lost exact rational or invented an "
        "empty mean");
  covered.metric = a::ProfileMetric::total_ticks;
  panel.set_request(doc, 0, covered);
  check(panel.series(0)->samples[2].state ==
                a::ProfileSampleState::no_matches &&
            panel.series(0)->samples[2].exact_value == "0",
        "explicit coverage did not distinguish no_matches from not_collected");
  auto ns = query(a::ProfileMetric::total_nanoseconds);
  panel.set_request(doc, 0, ns);
  draw(panel, doc, width);
  near(a::plot_value(*panel.series(0)->samples[0].value), 200,
       "nanosecond metric conversion wrong");
  check(panel.plot_view().unit == "ns", "nanosecond unit group mislabeled");
  doc.set_scene(2);
  draw(panel, doc, width);
  check(panel.series(0)->samples.size() == 1 &&
            panel.series(0)->samples[0].source.scene_id == 2,
        "scene switch retained previous native windows");
  doc.set_scene(1);
  draw(panel, doc, width);
  check(panel.indexed_event_count() == initial_prefix &&
            panel.profile_index()->statistics().profile_packets == packets,
        "normal UI rendering reparsed profile packet history");
  check(first.overflow <= 1, "profile panel overflows narrow content width");
  while (panel.series_count() < 16)
    panel.add_series(doc, query());
  rejects([&] { panel.add_series(doc, query()); });
  check(panel.erase_series(15) && panel.series_count() == 15,
        "series limit/delete incorrect");
  panel.reset();
  check(panel.series_count() == 0 && !panel.profile_index(),
        "reset retained a capture index");
  std::cout << "profile widgets width=" << width
            << " native mouse/zoom/pan/filter/unit/coverage passed\n";
}
void exact_csv(float width) {
  Fixture f(ByteOrder::little, false);
  f.frame(0, UINT64_MAX);
  Wire raw;
  raw.span(0, UINT64_MAX - 1, 1, 7);
  raw.span(0, UINT64_MAX - 1, 1, 8);
  f.packet(raw.bytes);
  Document doc;
  load(doc, f);
  ProfileCurvePanel panel;
  panel.add_series(doc, query());
  draw(panel, doc, width);
  auto frame = draw(panel, doc, width);
  const auto &style = ImGui::GetStyle();
  const char *caption =
      "Native scene frame windows: completed scopes are assigned by original "
      "end tick, never by the packet receipt frame. Inclusive totals include "
      "nested / overlapping threads; this is not CPU utilization.";
  const float wrapped =
      ImGui::CalcTextSize(caption, nullptr, false, frame.content_width).y;
  ImVec2 export_button{
      style.WindowPadding.x + ImGui::CalcTextSize("Refresh").x +
          2 * style.FramePadding.x + style.ItemSpacing.x +
          ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
          ImGui::CalcTextSize("Auto (1 Hz)").x + style.ItemSpacing.x + 8,
      style.WindowPadding.y + wrapped + style.ItemSpacing.y +
          ImGui::GetFrameHeight() * .5f};
  const auto exported = click(panel, doc, width, export_button);
  check(exported.csv.find("36893488147419103228") != std::string::npos &&
            exported.csv.find("18446744073709551615") != std::string::npos,
        "actual CSV button lost exact wide ticks or raw uint64 frame end");
  check(exported.csv.find("total_ticks") != std::string::npos &&
            exported.csv.find("value_exact") != std::string::npos,
        "exact CSV schema missing");
}

void frame_timing_widgets(float width) {
  StreamHeader header;
  header.timestamp_numerator=7;header.timestamp_denominator=3;
  std::vector<Event> events;
  const auto emit=[&](Payload value){events.push_back({1,999999,0,std::move(value)});};
  emit(StringHandleEvent{"physx3",1});emit(StringHandleEvent{"PxScene",2});
  emit(StringHandleEvent{"frame",3});emit(StringHandleEvent{"validation_after_fetch",4});
  emit(CreateClass{{1,2}});emit(CreateInstance{{1,2},1});
  for(const auto times:{std::pair<std::uint64_t,std::uint64_t>{100,105},{200,210},{300,320}}){emit(BeginSection{1,3,times.first});emit(EndSection{1,3,times.second});}
  emit(BeginSection{1,4,1000});emit(EndSection{1,4,1001});
  Document doc;doc.adopt(std::make_unique<Session>(header,events));doc.set_scene(1);
  ProfileCurvePanel panel;
  const auto click_control=[&](ProfileCurveControlRect r){
    check(r.width>0&&r.height>0,"actual frame-time widget rect unavailable");
    return click(panel,doc,width,{r.left+r.width*.5f,r.top+r.height*.5f});
  };
  draw(panel,doc,width);draw(panel,doc,width);
  click_control(panel.controls_view().metric);
  draw(panel,doc,width);
  click_control(panel.controls_view().frame_ticks);
  draw(panel,doc,width);draw(panel,doc,width);
  click_control(panel.controls_view().add);
  check(panel.series_count()==1&&panel.request(0)->metric==a::ProfileMetric::native_frame_ticks,
        "actual Metric/Add UI did not select native frame timing");
  check(panel.profile_index()->zones().empty()&&panel.series(0)->frame_timing&&
            panel.row_count()==3&&panel.series(0)->samples[0].exact_value=="5"&&
            panel.series(0)->samples[1].exact_value=="10"&&
            panel.series(0)->samples[2].exact_value=="20",
        "frame-time UI required CPU data or used receipt/FPS/afterfetch time");
  draw(panel,doc,width);const auto initial=draw(panel,doc,width);
  check(panel.plot_view().unit=="ticks"&&panel.visible_row_count()==3&&
            initial.text.find("Native frame wall-clock duration")!=std::string::npos,
        "native frame plot label/unit/points");
  check(doc.set_sample_point(SamplePoint::after_fetch),"narrow after-fetch marker setup");
  draw(panel,doc,width);
  click(panel,doc,width,screen_point(panel.plot_view(),2,10));
  check(doc.sample_point()==SamplePoint::native_frame&&doc.frame_index()==1,
        "frame-time mouse point did not seek original native frame");
  const auto old=panel.plot_view();
  draw(panel,doc,width,screen_point(old,2,10),{},1);draw(panel,doc,width);
  check(panel.plot_view().x_max-panel.plot_view().x_min<old.x_max-old.x_min,
        "native frame-time wheel zoom missing");
  panel.set_time_axis(true);draw(panel,doc,width);
  check(panel.plot_view().time_axis&&panel.plot_view().x_max<1e-5,
        "frame-time x axis used playback FPS");
  panel.add_frame_timing(doc,a::FrameTimingMetric::nanoseconds);
  const auto *data=panel.series(1);
  check(data&&data->samples[0].exact_value=="35/3"&&data->frame_timing,
        "frame-time UI lost exact rational ns");
  draw(panel,doc,width);draw(panel,doc,width);
  const auto exported=click_control(panel.controls_view().export_csv);
  check(exported.csv.find("duration_ticks")!=std::string::npos&&
            exported.csv.find("35/3")!=std::string::npos&&
            exported.csv.find("completed_count")==std::string::npos,
        "actual frame-time CSV button reused misleading CPU scope schema");
  panel.set_time_axis(false);const auto final=draw(panel,doc,width);
  check(initial.overflow<=1&&final.overflow<=1,"frame-time controls overflow width");
  std::cout<<"native frame-time widgets width="<<width<<" no-CPU/seek/zoom/time/CSV passed\n";
}
void live_refresh_and_bounds() {
  Fixture f;
  Document live;
  live.begin_live(f.header);
  live.append_live(std::vector<Event>(
      f.events.begin(),
      f.events.begin() + static_cast<std::ptrdiff_t>(f.late_prefix)));
  ProfileCurvePanel panel;
  auto q = query();
  q.partial = a::ProfilePartial::observed_subset;
  panel.add_series(live, q);
  const auto *persistent = panel.profile_index();
  const auto prefix = panel.indexed_event_count(),
             packets = persistent->statistics().profile_packets;
  check(panel.series(0)->provisional &&
            panel.series(0)->samples[0].total_ticks == "0",
        "received live prefix fabricated delayed scope data");
  live.append_live(std::vector<Event>(
      f.events.begin() + static_cast<std::ptrdiff_t>(f.late_prefix),
      f.events.end()));
  for (int i = 0; i < 20; ++i)
    draw(panel, live);
  check(panel.indexed_event_count() == prefix &&
            panel.series(0)->source_event_count == prefix &&
            panel.profile_index()->statistics().profile_packets == packets,
        "UI draw reparsed/resampled live history without a refresh");
  panel.set_auto_refresh(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(1050));
  draw(panel, live);
  check(panel.profile_index() == persistent &&
            panel.profile_index()->statistics().profile_packets ==
                packets + 1 &&
            panel.series(0)->samples[0].total_ticks == "40" &&
            panel.series(0)->samples[3].total_ticks == "350",
        "bounded auto refresh failed to increment persistent index and revise "
        "history");
  live.finish_live();
  panel.refresh(live);
  check(!panel.series(0)->provisional && panel.series(0)->samples[0].state ==
                                             a::ProfileSampleState::observed,
        "same-prefix finalization left curves provisional");
  Document partial;
  partial.begin_live(f.header);
  partial.append_live(std::vector<Event>(
      f.events.begin(),
      f.events.begin() + static_cast<std::ptrdiff_t>(f.late_prefix)));
  ProfileCurvePanel safe;
  safe.add_series(partial, q);
  const auto valid = safe.indexed_event_count();
  partial.append_live({Event{
      1, 0, 0,
      BeginSetPropertyValue{10,
                            f.handles.at("events"),
                            {f.handles.at("physx3"), f.handles.at("PvdU8")}}}});
  rejects([&] { safe.refresh(partial); });
  check(safe.indexed_event_count() == valid,
        "incomplete transaction changed profile index prefix");
  Document replacement;
  load(replacement, f);
  draw(panel, replacement);
  check(panel.series_count() == 0 && panel.row_count() == 0 &&
            panel.indexed_event_count() == replacement.session()->event_count(),
        "source switch retained prior series/index prefix");
  Fixture large(ByteOrder::little, false);
  for (std::uint64_t i = 0; i < 2100; ++i)
    large.frame(100 + i * 100, 200 + i * 100);
  Document many;
  load(many, large);
  ProfileCurvePanel capped;
  auto count = query(a::ProfileMetric::count);
  count.coverage = a::ProfileCoverage::assume_continuous;
  capped.add_series(many, count);
  check(capped.series(0)->samples.size() == 2000 &&
            capped.series(0)->resampled &&
            std::any_of(capped.series(0)->samples.begin(),
                        capped.series(0)->samples.end(),
                        [](const auto &s) { return s.break_before; }),
        "panel exceeded 2000 rows or interpolated skipped frames");
  std::cout << "profile persistent live refresh/partial-boundary/source "
               "reset/budgets passed\n";
}
void sdk(const char *capture) {
  Document doc;
  doc.open_file(capture);
  ProfileCurvePanel panel;
  draw(panel, doc);
  check(panel.profile_index(), "real profile panel index missing");
  a::ProfileCurveRequest q;
  for (const auto &z : panel.profile_index()->zones())
    if (z.second.name == "Frame oracle CPU")
      q.filter.zone = z.first;
  check(q.filter.zone.has_value(), "SDK oracle CPU zone missing");
  q.metric = a::ProfileMetric::total_nanoseconds;
  panel.add_series(doc, q);
  draw(panel, doc);
  check(panel.series(0)->samples.size() == 6 &&
            panel.visible_row_count() == 5 &&
            panel.series(0)->samples[4].state ==
                a::ProfileSampleState::not_collected,
        "real native SDK curve/gap count mismatch");
  const auto source = panel.series(0)->samples[2].source;
  check(panel.seek_sample(doc, 0, 2) &&
            doc.frame()->snapshot.event_count == source.event_count &&
            doc.frame_index() == 2,
        "real SDK native point seek wrong");
  check(panel.error().empty(), "real SDK profile curve panel error");
  std::cout << "real SDK profile panel 6 windows / 5 observed points passed\n";
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
    widgets(1060);
    widgets(420);
    exact_csv(1060);
    exact_csv(420);
    frame_timing_widgets(1060);
    frame_timing_widgets(420);
    live_refresh_and_bounds();
    if (argc > 1)
      sdk(argv[1]);
    ImGui::DestroyContext();
    std::cout << "profile curve panel tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "profile curve panel tests failed: " << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
