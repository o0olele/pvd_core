#include "analysis_panels.hpp"
#include "imgui_internal.h"
#include "ui_scale.hpp"
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture {
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  static void u(Bytes &bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  void emit(Payload payload) {
    events.push_back({1, events.size(), events.size() * 7, std::move(payload)});
  }
  std::uint32_t str(const std::string &s) {
    auto found = strings.find(s);
    if (found != strings.end())
      return found->second;
    auto id = static_cast<std::uint32_t>(strings.size() + 1);
    strings.emplace(s, id);
    emit(StringHandleEvent{s, id});
    return id;
  }
  WireName type(const char *n) {
    auto ns = str("physx3"), name = str(n);
    return {ns, name};
  }
  static void dictionary(Bytes &bytes, const char *text, unsigned handle) {
    const std::string s(text);
    u(bytes, 0x3ff1, 2);
    u(bytes, s.size() + 1, 4);
    bytes.insert(bytes.end(), s.begin(), s.end());
    u(bytes, 0, 1);
    u(bytes, handle, 4);
  }
  static void alloc(Bytes &bytes, std::uint64_t address, std::uint32_t size) {
    u(bytes, 0x3ff2, 2);
    u(bytes, size, 4);
    u(bytes, 1, 4);
    u(bytes, 2, 4);
    u(bytes, 42, 4);
    u(bytes, address, 8);
  }
  static void free(Bytes &bytes, std::uint64_t address) {
    u(bytes, 0x33, 2);
    u(bytes, address, 8);
  }
  void packet(Bytes bytes) {
    auto p = str("events");
    auto t = type("PvdU8");
    emit(SetPropertyValue{10, p, t, static_cast<std::uint32_t>(bytes.size()),
                          std::move(bytes)});
  }
  void mark(bool begin, const char *label) {
    auto h = str(label);
    if (begin)
      emit(BeginSection{1, h, events.size()});
    else
      emit(EndSection{1, h, events.size()});
  }
  Fixture() {
    const auto scene = type("PxScene"), memory = type("MemoryBuffer");
    emit(CreateClass{scene});
    emit(CreateClass{memory});
    auto p = str("events"), semantic = str("memory event stream");
    auto t = type("PvdU8");
    emit(CreateProperty{memory, p, semantic, t, 2, {}});
    emit(CreateInstance{scene, 1});
    emit(CreateInstance{memory, 10});
    mark(true, "frame");
    Bytes first;
    dictionary(first, "WindowUI", 1);
    dictionary(first, "window_ui.cpp", 2);
    alloc(first, 100, 64);
    packet(first);
    mark(false, "frame");
    mark(true, "validation_after_fetch");
    mark(false, "validation_after_fetch");
    Bytes before;
    free(before, 100);
    alloc(before, 200, 128);
    packet(before);
    mark(true, "frame");
    mark(false, "frame");
    mark(true, "validation_after_fetch");
    Bytes late;
    alloc(late, 300, 256);
    packet(late);
    mark(false, "validation_after_fetch");
    mark(true, "frame");
    Bytes last;
    free(last, 300);
    packet(last);
    mark(false, "frame");
    mark(true, "validation_after_fetch");
    mark(false, "validation_after_fetch");
  }
  void load(Document &doc) {
    StreamHeader h;
    h.stream_flags = 1;
    doc.adopt(std::make_unique<Session>(h, events));
  }
};
struct Harness {
  Document doc;
  AnalysisPanels panel;
  float scale;
  std::string text;
  ImGuiWindow *window = nullptr;
  explicit Harness(float s) : scale(s) {
    Fixture f;
    f.load(doc);
  }
  void frame(std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
    UiPixelScope scope(scale);
    auto &io = ImGui::GetIO();
    io.DisplaySize = {1060 * scale, 700 * scale};
    io.DeltaTime = 1.f / 60;
    if (mouse)
      io.AddMousePosEvent(mouse->x, mouse->y);
    if (down)
      io.AddMouseButtonEvent(0, *down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Memory native receipt window", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::LogToBuffer();
    panel.draw(doc, AnalysisPage::memory, {});
    text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    window = ImGui::GetCurrentWindow();
    check(window->DC.CursorMaxPos.x <= window->InnerRect.Max.x + 2 * scale,
          "frame memory summary fits scaled logical width");
    ImGui::End();
    ImGui::Render();
    check(ImGui::GetDrawData()->TotalVtxCount > 0,
          "real ImGui memory panel draws");
    check(text.find("Analysis error:") == std::string::npos,
          "memory frame panel has no hidden failure");
  }
  void stable() {
    frame();
    frame();
  }
  void click_current() {
    const auto &style = ImGui::GetStyle();
    const ImVec2 point{style.WindowPadding.x + ImGui::CalcTextSize("MEMORY").x +
                           style.ItemSpacing.x + ImGui::GetFrameHeight() * .5f,
                       style.WindowPadding.y + ImGui::GetFrameHeight() * .5f};
    frame(point, false);
    frame(point, true);
    frame(point, false);
    frame(point, false);
  }
};
void run(float scale) {
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = io.LogFilename = nullptr;
  UiScale fonts(ImGui::GetStyle());
  fonts.reload_fonts(scale);
  Harness h(scale);
  h.stable();
  auto stats = h.panel.memory_window_statistics();
  check(stats && stats->activity.allocated_bytes == 64 &&
            stats->activity.freed_bytes == 0 &&
            stats->as_of_end.current_bytes == 64 &&
            stats->activity.churn_bytes() == "64",
        "first native frame stats are actual receipt prefix");
  check(h.text.find("Global memory events received") != std::string::npos &&
            h.text.find("no scene ownership or allocation timestamps") !=
                std::string::npos &&
            h.text.find("Allocated(frame): 64 B") != std::string::npos,
        "actual UI names receipt scope and exact byte statistics");
  h.doc.set_frame(1);
  h.stable();
  stats = h.panel.memory_window_statistics();
  check(stats && stats->activity.allocated_bytes == 128 &&
            stats->activity.freed_bytes == 64 &&
            stats->as_of_end.current_bytes == 128 &&
            stats->activity.churn_bytes() == "192" &&
            stats->activity.net_bytes() == "64",
        "next native frame uses preceding native end, not begin section");
  check(h.doc.set_sample_point(SamplePoint::after_fetch),
        "real explicit post-fetch timeline");
  check(h.doc.set_frame(1), "explicit second post-fetch sample");
  h.stable();
  stats = h.panel.memory_window_statistics();
  check(stats && stats->as_of_end.current_bytes == 128 &&
            stats->activity.allocated_bytes == 128 &&
            h.text.find("After-fetch view") != std::string::npos,
        "post-fetch late packet is explicitly excluded from native receipt "
        "interval");
  h.click_current();
  stats = h.panel.memory_window_statistics();
  check(stats && stats->activity.allocated_bytes == 128,
        "history cumulative-mode toggle never redefines frame window");
  h.doc.set_sample_point(SamplePoint::native_frame);
  h.doc.set_frame(2);
  h.stable();
  stats = h.panel.memory_window_statistics();
  check(stats && stats->activity.allocated_bytes == 256 &&
            stats->activity.freed_bytes == 256 &&
            stats->as_of_end.current_bytes == 128 &&
            stats->activity.churn_bytes() == "512" &&
            stats->activity.net_bytes() == "0",
        "late allocate/free pair is received in later window even when both "
        "lifetime ends share prefix history");
  h.doc.set_frame(0);
  h.stable();
  stats = h.panel.memory_window_statistics();
  check(stats && stats->as_of_end.current_bytes == 64 &&
            stats->activity.allocated_bytes == 64,
        "reverse seek restores earlier flow and outstanding without future "
        "leakage");
  std::cout << "Actual memory frame UI scale " << scale << " passed\n";
  ImGui::DestroyContext();
}
} // namespace
int main() {
  try {
    for (float scale : {1.f, 1.5f, 2.f})
      run(scale);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
