#include "imgui.h"
#include "imgui_internal.h"
#include "panels.hpp"
#include "ui_scale.hpp"
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
    p.integer_hex = true;
    check(propertyValueText(large, 0, p) == "0xFEDCBA9876543210" &&
              propertyValueText(negative, 0, p) == "-0x7FFFFFFFFFFFFFF9",
          "exact unsigned and signed-magnitude hexadecimal formatting");
    auto minimum = property("PvdI64", integer(0x8000000000000000ull, 8, order), order);
    check(propertyValueText(minimum, 0, p) == "-0x8000000000000000",
          "INT64_MIN hexadecimal avoids signed overflow");
    p.integer_hex = false;
    Bytes raw_position, native_position;
    for (const auto v : {100., 5., 0.}) {
      auto part = fp(v, false, order);
      raw_position.insert(raw_position.end(), part.begin(), part.end());
    }
    for (const auto v : {0., 3., 3.}) {
      auto part = fp(v, false, order);
      native_position.insert(native_position.end(), part.begin(), part.end());
    }
    auto shifted = property("PxVec3", raw_position, order);
    shifted.native_coordinate_bytes =
        std::make_shared<const Bytes>(std::move(native_position));
    check(propertyValueText(shifted).find("100") == std::string::npos &&
              propertyValueText(shifted, 0, {}, true).find("100") !=
                  std::string::npos,
          "Inspector defaults to native coordinates and exposes last-wire value");
    check(shifted.raw_bytes() == raw_position,
          "native inspection cannot modify source bytes");
    check(propertyValueCopyText(shifted, 0).find("100") == std::string::npos &&
              propertyValueCopyText(shifted, 0, PropertyCopy::last_wire_value)
                  .find("100") != std::string::npos &&
              propertyValueCopyText(shifted, 0, PropertyCopy::wire_bytes) ==
                  propertyValueCopyText(property("PxVec3", raw_position, order),
                                        0, PropertyCopy::wire_bytes),
          "clipboard native/wire/byte coordinates remain explicit");
    InspectorPreferences low;
    low.number_format = NumberFormat::fixed;
    low.double_precision = 1;
    check(std::stod(propertyValueCopyText(precise, 0, PropertyCopy::native_value, low)) ==
              1.2345678901234567 &&
              std::stof(propertyValueCopyText(small, 0, PropertyCopy::native_value, low)) ==
                  static_cast<float>(1e-12),
          "copy does not lose real values through row precision");
    PropertyDefinition symbols;
    symbols.semantic = "Enumeration Value";
    symbols.named_values = {{"eA", 1}, {"eB", 2}, {"eZero", 0}};
    auto combined = property("PvdU64", integer(3, 8, order), order);
    check(propertyValueText(combined, 0, {}, false, &symbols) == "Unknown (3)",
          "enum values are never inferred as bitflags");
    auto zero = property("PvdU32", integer(0, 4, order), order);
    check(propertyValueText(zero, 0, {}, false, &symbols) == "eZero (0)",
          "zero enum mapping remains visible");
    symbols.semantic = "Bitflag";
    auto unknown = property("PvdU64", integer(0x8000000000000003ull, 8, order), order);
    const auto flags = propertyValueText(unknown, 0, {}, false, &symbols);
    check(flags == "eA | eB | Unknown bits 0x8000000000000000 (9223372036854775811)",
          "bitflag symbols preserve exact uint64 and all unknown bits");
    p.integer_hex = true;
    check(propertyValueText(unknown, 0, p, false, &symbols).find(
              "(0x8000000000000003)") != std::string::npos,
          "symbolic raw annotation obeys exact hexadecimal preference");
    p.integer_hex = false;
    Bytes selected_bytes = integer(1, 4, order);
    auto second = integer(2, 4, order);
    selected_bytes.insert(selected_bytes.end(), second.begin(), second.end());
    auto selected = property("PvdU32", selected_bytes, order);
    selected.item_count = 2;
    check(propertyValueCopyText(selected, 1, PropertyCopy::native_value, {}, &symbols) ==
              "eB (2)" &&
              propertyValueCopyText(selected, 1, PropertyCopy::wire_bytes) ==
                  (order == ByteOrder::little ? "02 00 00 00" : "00 00 00 02"),
          "array copy selects original element and incoming byte order");
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
  auto long_text = property("String", {});
  long_text.text = std::string(7000, 'x') + "\nend";
  check(propertyValueText(long_text).size() < long_text.text->size() &&
            propertyValueCopyText(long_text, 0) == *long_text.text,
        "copy retains complete long strings and newlines beyond row preview");
  long_text.text = std::string("before\0after", 12);
  check(propertyValueCopyText(long_text, 0) == "before\\u0000after",
        "embedded NUL cannot silently truncate clipboard content");
  Bytes opaque(64);
  for (std::size_t i = 0; i < opaque.size(); ++i)
    opaque[i] = static_cast<std::uint8_t>(i);
  auto blob = property("UnknownOpaque", opaque);
  check(propertyValueText(blob).find(" ...") != std::string::npos &&
            propertyValueCopyText(blob, 0).size() == 64 * 3 - 1 &&
            propertyValueCopyText(blob, 0).find("3E 3F") != std::string::npos,
        "opaque copy retains all selected bytes");
  blob.item_count = 3;
  bool rejected = false;
  try { (void)propertyValueCopyText(blob, 0, PropertyCopy::wire_bytes); }
  catch (const std::invalid_argument &) { rejected = true; }
  check(rejected, "ambiguous opaque element copy fails explicitly");
  rejected = false;
  try { (void)propertyValueCopyText(property("PvdU64", Bytes{1}), 0); }
  catch (const std::exception &) { rejected = true; }
  check(rejected, "incomplete typed data cannot be copied as a true value");
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
  void prop(const char *name, const char *t, bool array = false,
            const char *semantic = "",
            std::initializer_list<std::pair<const char *, std::uint32_t>> named = {}) {
    auto cls = type("NumberObject"), dt = type(t);
    auto h = str(name);
    std::vector<NameHandleValue> values;
    for (const auto &entry : named)
      values.push_back({str(entry.first), entry.second});
    emit(CreateProperty{cls, h, *semantic ? str(semantic) : 0, dt,
                        static_cast<std::uint8_t>(array ? 2 : 1), values});
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
    prop("FEnum", "PvdU32", false, "Enumeration Value", {{"eA", 1}, {"eB", 2}});
    prop("GFlags", "PvdU64", false, "Bitflag", {{"eA", 1}, {"eB", 2}});
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
    value("FEnum", "PvdU32", integer(2));
    value("GFlags", "PvdU64", integer(0x8000000000000003ull, 8));
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
             bool log = false, int mouse_button = 0) {
    auto &io = ImGui::GetIO();
    io.DisplaySize = {width, 900};
    io.DeltaTime = 1.f / 60;
    if (mouse)
      io.AddMousePosEvent(mouse->x, mouse->y);
    if (down)
      io.AddMouseButtonEvent(mouse_button, *down);
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
  void context_click(ImVec2 p) {
    frame(p, false, false, 1);
    frame(p, true, false, 1);
    frame(p, false, false, 1);
    frame(p, false, false, 1);
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
    return ImGui::WindowRectRelToAbs(
        g.NavWindow, g.NavWindow->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
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
            h.text.find("1.2345678901234567") != std::string::npos &&
            h.text.find("eB (2)") != std::string::npos &&
            h.text.find("Unknown bits 0x8000000000000000") != std::string::npos,
        "real inspector displays exact integer and default double");
  const auto original = h.doc.frame()
                            ->snapshot.instance(object_id)
                            ->property("BFloat")
                            ->raw_bytes();
  // Logging temporarily expands array trees. Let the following ordinary
  // frames settle the scrollbar before measuring this compact header button.
  h.frame();
  h.frame();
  const auto &style = ImGui::GetStyle();
  const float settings_width = ui_pixels(24);
  const ImVec2 settings_button{
      h.window->WorkRect.Max.x - settings_width * .5f,
      h.window->DC.CursorStartPos.y + settings_width * .5f};
  h.frame(settings_button, false);
  check(ImGui::GetCurrentContext()->HoveredId ==
            h.window->GetID("Settings##Inspector settings"),
        "settings button is mouse-accessible at the inspector header");
  h.click(settings_button);
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
  h.focus(preferences, "Integer hexadecimal");
  h.click(h.focused());
  check(h.panel.inspector_preferences.integer_hex,
        "real mouse integer hexadecimal preference");
  h.focus(preferences, "Integer hexadecimal");
  h.click(h.focused());
  h.choose(preferences, "Array group size", 3);
  check(h.panel.inspector_preferences.array_page_size == 2048,
        "real mouse array grouping selection");
  h.key(ImGuiKey_Escape);
  h.frame();
  const auto table_id = h.window->GetID("##properties");
  auto *table = ImGui::GetCurrentContext()->Tables.GetByKey(table_id);
  check(table, "real inspector property table");
  const auto row = ImGui::GetTextLineHeight() + 2 * style.CellPadding.y;
  std::vector<std::string> copied;
  h.panel.copy_text = [&](const std::string &value) { copied.push_back(value); };
  h.context_click({table->Columns[1].WorkMinX + 12,
                   table->OuterRect.Min.y + row * 4.5f});
  auto *copy_menu = h.popup();
  h.focus(copy_menu, "Copy native value");
  h.click(h.focused());
  check(copied.size() == 1 && copied.back() == "18364758544493064720",
        "real property context command copies exact selected uint64 through callback");
  h.frame();
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
  table = ImGui::GetCurrentContext()->Tables.GetByKey(table_id);
  h.context_click({table->Columns[1].WorkMinX + 5,
                   table->OuterRect.Min.y + row * 3.5f});
  copy_menu = h.popup();
  h.focus(copy_menu, "Copy native value");
  h.click(h.focused());
  check(copied.size() == 2 && copied.back() == "4096",
        "real array row copies original sparse-page element, not page offset");
  h.frame();
  h.context_click({table->Columns[1].WorkMinX + 5,
                   table->OuterRect.Min.y + row * 3.5f});
  copy_menu = h.popup();
  h.focus(copy_menu, "Copy wire bytes");
  h.click(h.focused());
  check(copied.size() == 3 && copied.back() == "00 10 00 00",
        "real array wire-byte command uses selected element only");
  h.frame();
  check(h.doc.frame()
                ->snapshot.instance(object_id)
                ->property("BFloat")
                ->raw_bytes() == original,
        "UI preference changes preserve source fields");
  check(h.overflow <= 1, "inspector fits available width");
  h.timeline = true;
  h.frame();
  h.frame();
  h.focus(h.window, "...##playback-options");
  h.click(h.focused());
  auto *playback_options = h.popup();
  h.focus(playback_options, "Reverse");
  h.click(h.focused());
  check(h.doc.playback().frames_per_second < 0,
        "real mouse Reverse toggle updates signed playback");
  h.key(ImGuiKey_Escape);
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
