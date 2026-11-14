#include "imgui.h"
#include "imgui_internal.h"
#include "panels.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
Bytes integer(std::uint64_t n, std::size_t width = 4,
              ByteOrder order = ByteOrder::little) {
  Bytes out;
  for (std::size_t i = 0; i < width; ++i)
    out.push_back(static_cast<std::uint8_t>(
        n >> ((order == ByteOrder::little ? i : width - i - 1) * 8)));
  return out;
}
Bytes fp(double n, bool wide, ByteOrder order = ByteOrder::little) {
  if (wide) {
    std::uint64_t bits;
    std::memcpy(&bits, &n, 8);
    return integer(bits, 8, order);
  }
  float v = static_cast<float>(n);
  std::uint32_t bits;
  std::memcpy(&bits, &v, 4);
  return integer(bits, 4, order);
}
PropertyValue property(const char *type, Bytes bytes,
                       ByteOrder order = ByteOrder::little) {
  PropertyValue v;
  v.incoming_type = {"physx3", type};
  v.item_count = 1;
  v.byte_order = order;
  v.pointer_width = 8;
  v.bytes = std::make_shared<const Bytes>(std::move(bytes));
  return v;
}
void formatting() {
  InspectorPreferences p;
  check(p.float_precision == 9 && p.double_precision == 17 &&
            valid_inspector_preferences(p),
        "round-trip defaults");
  for (auto order : {ByteOrder::little, ByteOrder::big}) {
    auto small = property("PvdF32", fp(1e-12, false, order), order),
         precise =
             property("PvdF64", fp(1.2345678901234567, true, order), order);
    check(std::stof(propertyValueText(small)) == static_cast<float>(1e-12),
          "small float retains general precision");
    check(std::stod(propertyValueText(precise)) == 1.2345678901234567,
          "17-digit double retains round-trip precision");
    const auto original = small.raw_bytes();
    auto large = property("PvdU64", integer(0xfedcba9876543210ull, 8, order),
                          order),
         negative = property("PvdI64", integer(0x8000000000000007ull, 8, order),
                             order);
    for (auto format : {NumberFormat::general, NumberFormat::scientific,
                        NumberFormat::fixed}) {
      p.number_format = format;
      p.float_precision = p.double_precision = 17;
      check(propertyValueText(large, 0, p) == "18364758544493064720" &&
                propertyValueText(negative, 0, p) == "-9223372036854775801",
            "64-bit integer never passes through floating display format");
      auto text = propertyValueText(small, 0, p);
      check(std::stod(text) > 0,
            "small value not fabricated as zero at sufficient precision");
      if (format == NumberFormat::scientific)
        check(text.find("e-") != std::string::npos,
              "scientific notation applied");
      if (format == NumberFormat::fixed)
        check(text.find('e') == std::string::npos &&
                  text.find("000000000001") != std::string::npos,
              "fixed decimal notation applied");
    }
    check(small.raw_bytes() == original,
          "preferences cannot mutate source bytes");
  }
  for (auto group : {32u, 128u, 512u, 2048u}) {
    p.array_page_size = group;
    check(valid_inspector_preferences(p), "supported group preference");
  }
  p.array_page_size = 8192;
  check(!valid_inspector_preferences(p), "group limit validated");
  p = {};
  p.double_precision = 18;
  check(!valid_inspector_preferences(p), "precision limit validated");
}
constexpr std::uint64_t object_id = 0xfedcba9876543210ull;
struct Fixture {
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  void emit(Payload p) {
    events.push_back({1, events.size(), events.size() * 9, std::move(p)});
  }
  std::uint32_t str(const std::string &s) {
    auto i = strings.find(s);
    if (i != strings.end())
      return i->second;
    auto h = static_cast<std::uint32_t>(strings.size() + 1);
    strings[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *n) {
    auto ns = str("physx3"), name = str(n);
    return {ns, name};
  }
  void prop(const char *name, const char *t, bool array = false) {
    auto cls = type("NumberObject"), dt = type(t);
    auto h = str(name);
    emit(CreateProperty{
        cls, h, 0, dt, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void value(const char *name, const char *t, Bytes bytes,
             std::uint32_t count = 1) {
    auto h = str(name);
    auto dt = type(t);
    emit(SetPropertyValue{object_id, h, dt, count, std::move(bytes)});
  }
  Fixture() {
    emit(CreateClass{type("PxScene")});
    emit(CreateClass{type("NumberObject")});
    for (auto pair : {std::pair<const char *, const char *>{"BFloat", "PvdF32"},
                      {"CDouble", "PvdF64"},
                      {"DInteger", "PvdU64"},
                      {"ESigned", "PvdI64"}})
      prop(pair.first, pair.second);
    prop("AArray", "PvdU32", true);
    emit(CreateInstance{type("PxScene"), 1});
    emit(CreateInstance{type("NumberObject"), object_id});
    Bytes array;
    for (unsigned i = 0; i < 4097; ++i) {
      auto b = integer(i);
      array.insert(array.end(), b.begin(), b.end());
    }
    value("AArray", "PvdU32", std::move(array), 4097);
    value("BFloat", "PvdF32", fp(1e-12, false));
    value("CDouble", "PvdF64", fp(1.2345678901234567, true));
    value("DInteger", "PvdU64", integer(object_id, 8));
    value("ESigned", "PvdI64", integer(0x8000000000000007ull, 8));
    for (unsigned i = 0; i < 6; ++i) {
      auto frame = str("frame");
      emit(BeginSection{1, frame, 100 + i * 100});
      emit(EndSection{1, frame, 200 + i * 100});
    }
  }
  void load(Document &doc) {
    StreamHeader h;
    h.stream_flags = 1;
    doc.adopt(std::make_unique<Session>(h, events));
    check(doc.inspect(object_id), "inspect actual high-ID object");
  }
};
struct Harness {
  Document doc;
  PanelState panel;
  float width;
  bool timeline = false;
  std::string text;
  float overflow = 0;
  ImGuiWindow *window = nullptr;
  explicit Harness(float w) : width(w) {
    Fixture f;
    f.load(doc);
  }
  void frame(std::optional<ImVec2> mouse = {}, std::optional<bool> down = {},
             bool log = false) {
    auto &io = ImGui::GetIO();
    io.DisplaySize = {width, 900};
    io.DeltaTime = 1.f / 60;
    if (mouse)
      io.AddMousePosEvent(mouse->x, mouse->y);
    if (down)
      io.AddMouseButtonEvent(0, *down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({width, 800});
    const auto name = std::string("Inspection preferences test ") +
                      std::to_string(static_cast<int>(width));
    ImGui::Begin(name.c_str(), nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    if (log)
      ImGui::LogToBuffer();
    if (timeline)
      drawTimeline(doc, panel);
    else
      drawInspector(doc, panel);
    if (log) {
      text = ImGui::GetCurrentContext()->LogBuffer.c_str();
      ImGui::LogFinish();
    }
    window = ImGui::GetCurrentWindow();
    overflow =
        std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
    ImGui::End();
    ImGui::Render();
    check(panel.error.empty(), panel.error.c_str());
  }
  void click(ImVec2 p) {
    frame(p, false);
    frame(p, true);
    frame(p, false);
    frame(p, false);
  }
  void key(ImGuiKey k) {
    ImGui::GetIO().AddKeyEvent(k, true);
    frame();
    ImGui::GetIO().AddKeyEvent(k, false);
    frame();
  }
  void focus(ImGuiWindow *target, const char *label) {
    const auto id = target->GetID(label);
    for (unsigned i = 0; i < 16; ++i) {
      const auto &g = *ImGui::GetCurrentContext();
      if (g.NavWindow == target && g.NavId == id)
        return;
      key(ImGuiKey_Tab);
    }
    throw std::runtime_error(std::string("keyboard could not focus ") + label);
  }
  ImVec2 focused() const {
    const auto &g = *ImGui::GetCurrentContext();
    auto p = g.NavWindow->NavRectRel[ImGuiNavLayer_Main].GetCenter();
    return {p.x + g.NavWindow->Pos.x, p.y + g.NavWindow->Pos.y};
  }
  ImGuiWindow *popup() {
    frame();
    const auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
    check(stack.Size && stack.back().Window, "real preference popup opened");
    return stack.back().Window;
  }
  void choose(ImGuiWindow *parent, const char *label, int index) {
    focus(parent, label);
    click(focused());
    auto *menu = popup();
    check(menu != parent, "real combo menu opened");
    const ImVec2 point{menu->DC.CursorStartPos.x + 20,
                       menu->DC.CursorStartPos.y +
                           ImGui::GetTextLineHeight() * .5f +
                           index * ImGui::GetTextLineHeightWithSpacing()};
    click(point);
  }
};
void widgets(float width) {
  Harness h(width);
  h.frame();
  h.frame({}, {}, true);
  check(h.text.find("18364758544493064720") != std::string::npos &&
            h.text.find("1.2345678901234567") != std::string::npos,
        "real inspector displays exact integer and default double");
  const auto original = h.doc.frame()
                            ->snapshot.instance(object_id)
                            ->property("BFloat")
                            ->raw_bytes();
  const auto &style = ImGui::GetStyle();
  h.click({style.WindowPadding.x + 25,
           style.WindowPadding.y + ImGui::GetTextLineHeight() * .5f});
  auto *preferences = h.popup();
  h.choose(preferences, "Number format", 1);
  check(h.panel.inspector_preferences.number_format == NumberFormat::scientific,
        "real mouse scientific format selection");
  h.choose(preferences, "Number format", 2);
  check(h.panel.inspector_preferences.number_format == NumberFormat::fixed,
        "real mouse fixed format selection");
  h.choose(preferences, "Number format", 0);
  check(h.panel.inspector_preferences.number_format == NumberFormat::general,
        "real mouse general format selection");
  h.choose(preferences, "Array group size", 3);
  check(h.panel.inspector_preferences.array_page_size == 2048,
        "real mouse array grouping selection");
  h.key(ImGuiKey_Escape);
  h.frame();
  const auto table_id = h.window->GetID("##properties");
  auto *table = ImGui::GetCurrentContext()->Tables.GetByKey(table_id);
  check(table, "real inspector property table");
  const auto row = ImGui::GetTextLineHeight() + 2 * style.CellPadding.y;
  h.click(
      {table->Columns[0].WorkMinX + 6, table->OuterRect.Min.y + row * 1.5f});
  check(h.panel.array_visible_rows > 0, "real array tree expands");
  check(h.panel.array_visible_rows < 128,
        "2048 rows remain clipped to viewport");
  const std::string page = std::to_string(object_id) + ":0:AArray";
  for (auto group : {32u, 128u, 512u, 2048u}) {
    h.panel.inspector_preferences.array_page_size = group;
    h.panel.array_pages[page] = 1;
    h.frame({}, {}, true);
    const auto range =
        std::to_string(group + 1) + "-" + std::to_string(group * 2) + " / 4097";
    check(h.text.find(range) != std::string::npos,
          "array group navigates correct original source range");
    h.frame();
    check(h.panel.array_visible_rows <= group &&
              h.panel.array_visible_rows < 128,
          "large group viewport rows bounded");
  }
  h.panel.array_pages[page] = 999;
  h.frame({}, {}, true);
  check(h.text.find("4097-4097 / 4097") != std::string::npos,
        "array last page is bounded without inventing elements");
  check(h.doc.frame()
                ->snapshot.instance(object_id)
                ->property("BFloat")
                ->raw_bytes() == original,
        "UI preference changes preserve source fields");
  check(h.overflow <= 1, "inspector fits available width");
  h.timeline = true;
  h.frame();
  h.frame();
  h.focus(h.window, "Reverse");
  h.click(h.focused());
  check(h.doc.playback().frames_per_second < 0,
        "real mouse Reverse toggle updates signed playback");
  h.doc.set_frame(4);
  h.doc.set_playing(true);
  h.doc.advance(1. / 60);
  check(h.doc.frame_index() == 3, "Reverse UI causes real backwards playback");
  h.doc.set_playing(false);
  h.focus(h.window, "fps");
  const auto fps = h.focused();
  h.frame(fps, false);
  h.frame(fps, true);
  h.frame(ImVec2{fps.x + 15, fps.y}, true);
  h.frame(ImVec2{fps.x + 15, fps.y}, false);
  check(h.doc.playback().frames_per_second < 0,
        "speed editing preserves Reverse direction");
  h.frame({}, {}, true);
  check(h.overflow <= 1, "timeline direction controls fit available width");
  std::cout << "Real inspector/group/reverse ImGui width=" << width
            << " passed\n";
}
} // namespace
int main() {
  try {
    formatting();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontDefault();
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    widgets(1060);
    widgets(420);
    ImGui::DestroyContext();
    std::cout << "Inspection preference tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
