// Runs actual Dear ImGui widgets without a renderer/backend. The fixture uses
// real core Session events; no panel implementation or analysis index is
// mocked.
#include "analysis_panels.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
void check(bool b, const std::string &m) {
  if (!b)
    throw std::runtime_error(m);
}
struct Fixture {
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  void emit(Payload p) {
    events.push_back({1, events.size(), events.size() * 4, std::move(p)});
  }
  std::uint32_t str(const std::string &s) {
    auto it = handles.find(s);
    if (it != handles.end())
      return it->second;
    auto h = static_cast<std::uint32_t>(handles.size() + 1);
    handles[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const std::string &n) {
    auto ns = str("physx3"), name = str(n);
    return {ns, name};
  }
  static void u(Bytes &b, std::uint64_t n, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
  }
  static void string(Bytes &b, const std::string &s, std::uint32_t h) {
    u(b, 0x3ff1, 2);
    u(b, s.size() + 1, 4);
    b.insert(b.end(), s.begin(), s.end());
    u(b, 0, 1);
    u(b, h, 4);
  }
  static void allocation(Bytes &b, std::uint64_t addr, std::uint32_t size) {
    u(b, 0x3ff2, 2);
    u(b, size, 4);
    u(b, 1, 4);
    u(b, 2, 4);
    u(b, 42, 4);
    u(b, addr, 8);
  }
  Fixture() {
    emit(CreateClass{type("PxScene")});
    emit(CreateClass{type("MemoryFixture")});
    auto cls = type("MemoryFixture"), dt = type("PvdU8");
    auto name = str("events"), semantic = str("memory event stream");
    emit(CreateProperty{cls, name, semantic, dt, 2, {}});
    emit(CreateInstance{type("PxScene"), 1});
    emit(CreateInstance{cls, 10});
    auto frame = str("frame");
    for (unsigned phase = 0; phase != 2; ++phase) {
      emit(BeginSection{1, frame, events.size()});
      Bytes b;
      if (phase == 0) {
        string(b, "FixtureType", 1);
        string(b, "fixture.cpp", 2);
      }
      for (unsigned j = 0; j != 300; ++j)
        allocation(b, 0x10000 + (phase * 300 + j) * 16, phase ? 128 : 64);
      if (phase) {
        for (unsigned j = 0; j != 10; ++j) {
          u(b, 0x33, 2);
          u(b, 0x10000 + j * 16, 8);
          allocation(b, 0x10000 + j * 16, 256);
        }
      }
      emit(SetPropertyValue{10, name, dt, static_cast<std::uint32_t>(b.size()),
                            std::move(b)});
      emit(ErrorMessage{phase ? 128u : 2u,
                        phase ? "SECOND_FRAME_ERROR" : "FIRST_FRAME_ERROR",
                        "fixture.cpp", phase + 10});
      emit(EndSection{1, frame, events.size()});
    }
  }
};
struct Output {
  std::string text;
  float overflow_x = 0, scroll_y = 0;
};
Output draw(AnalysisPanels &panels, Document &document, AnalysisPage page,
            float width, std::optional<ImVec2> mouse = std::nullopt,
            std::optional<bool> button = std::nullopt) {
  auto &io = ImGui::GetIO();
  io.DisplaySize = {width, 900};
  io.DeltaTime = 1.0f / 60;
  if (mouse)
    io.AddMousePosEvent(mouse->x, mouse->y);
  if (button)
    io.AddMouseButtonEvent(0, *button);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({width, 760});
  ImGui::Begin("Analysis test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::LogToBuffer();
  panels.draw(document, page,
              [](const char *, const std::function<void(std::ostream &)> &) {});
  Output out;
  out.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  auto *w = ImGui::GetCurrentWindow();
  out.overflow_x = std::max(0.0f, w->DC.CursorMaxPos.x - w->InnerRect.Max.x);
  out.scroll_y = ImGui::GetScrollMaxY();
  ImGui::End();
  ImGui::Render();
  check(ImGui::GetDrawData() != nullptr, "ImGui did not produce draw data");
  return out;
}
void click_current_errors(AnalysisPanels &panels, Document &doc, float width) {
  const auto &style = ImGui::GetStyle();
  ImVec2 point{style.WindowPadding.x + ImGui::CalcTextSize("ERROR STREAM").x +
                   style.ItemSpacing.x + ImGui::GetFrameHeight() * .5f,
               style.WindowPadding.y + ImGui::GetFrameHeight() * .5f};
  draw(panels, doc, AnalysisPage::errors, width, point, false);
  draw(panels, doc, AnalysisPage::errors, width, point, true);
  draw(panels, doc, AnalysisPage::errors, width, point, false);
}
void synthetic(float width) {
  Fixture f;
  Document doc;
  StreamHeader header;
  header.stream_flags = 1;
  doc.adopt(std::make_unique<Session>(header, f.events), "synthetic analysis");
  check(doc.timeline().size() == 2, "synthetic source frames missing");
  doc.set_frame(0);
  AnalysisPanels panels;
  draw(panels, doc, AnalysisPage::memory, width);
  auto first = draw(panels, doc, AnalysisPage::memory, width);
  check(panels.visible_row_count(AnalysisPage::memory) == 300,
        "memory visible row API ignored first prefix");
  check(first.text.find("300 matching rows") != std::string::npos,
        "memory first frame prefix row count missing");
  check(first.text.find("Analysis error:") == std::string::npos,
        "memory panel surfaced an error");
  doc.set_frame(1);
  auto second = draw(panels, doc, AnalysisPage::memory, width);
  check(panels.visible_row_count(AnalysisPage::memory) == 610,
        "memory visible row API ignored second prefix");
  check(second.text.find("610 matching rows") != std::string::npos,
        "memory second frame prefix row count missing");
  check(second.text.find("Page 1 / 3") != std::string::npos,
        "memory history pagination wrong");
  doc.set_frame(0);
  auto backwards = draw(panels, doc, AnalysisPage::memory, width);
  check(panels.visible_row_count(AnalysisPage::memory) == 300,
        "memory visible row API leaked future allocations");
  check(backwards.text.find("300 matching rows") != std::string::npos &&
            backwards.text.find("Page 1 / 2") != std::string::npos,
        "reverse memory prefix leaked later rows");
  draw(panels, doc, AnalysisPage::errors, width);
  auto all = draw(panels, doc, AnalysisPage::errors, width);
  check(all.text.find("2 matching rows") != std::string::npos &&
            all.text.find("SECOND_FRAME_ERROR") != std::string::npos,
        "whole capture errors missing");
  click_current_errors(panels, doc, width);
  auto current = draw(panels, doc, AnalysisPage::errors, width);
  check(panels.visible_row_count(AnalysisPage::errors) == 1,
        "error visible row API ignored prefix");
  check(current.text.find("1 matching rows") != std::string::npos &&
            current.text.find("FIRST_FRAME_ERROR") != std::string::npos &&
            current.text.find("SECOND_FRAME_ERROR") == std::string::npos,
        "error current frame toggle/prefix failed");
  doc.set_frame(1);
  auto next = draw(panels, doc, AnalysisPage::errors, width);
  check(next.text.find("2 matching rows") != std::string::npos,
        "error frame advance failed");
  std::cout << "analysis layout width=" << width
            << " memory_overflow=" << second.overflow_x
            << " error_overflow=" << next.overflow_x << "\n";
  check(second.overflow_x <= 1 && next.overflow_x <= 1,
        "analysis controls overflow horizontal content region");
}
void real(const char *file) {
  Document doc;
  doc.open_file(file);
  AnalysisPanels panels;
  auto memory = draw(panels, doc, AnalysisPage::memory, 1440);
  auto errors = draw(panels, doc, AnalysisPage::errors, 1440);
  check(panels.row_count(AnalysisPage::memory) > 1000,
        "real SDK memory rows missing");
  check(panels.row_count(AnalysisPage::errors) >= 5,
        "real SDK error rows missing");
  check(memory.text.find("Analysis error:") == std::string::npos &&
            errors.text.find("MemoryFixture phase 4") != std::string::npos,
        "real SDK panel output failed");
  std::cout << "real analysis panels: "
            << panels.row_count(AnalysisPage::memory) << " memory rows, "
            << panels.row_count(AnalysisPage::errors) << " errors\n";
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
    synthetic(1440);
    synthetic(1060);
    if (argc > 1)
      real(argv[1]);
    ImGui::DestroyContext();
    std::cout << "analysis panel tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
