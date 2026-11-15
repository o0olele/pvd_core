#pragma once

#include "imgui.h"
#include "ui_scale.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace pvd::viewer::ui {

inline constexpr float font_size_dp = 17.f;
inline constexpr float menu_height_dp = 46.f;
inline constexpr float documents_height_dp = 42.f;
inline constexpr float viewport_bar_height_dp = 45.f;
inline constexpr float icon_size_dp = 16.f;
inline constexpr float control_height_dp = 30.f;

namespace detail {
inline constexpr int popup_style_vars = 7;
inline constexpr int popup_style_colors = 8;
} // namespace detail

// Toolbar rows deliberately use zero vertical spacing. A popup must not inherit
// that compact layout: keep its own padding, row spacing and opaque surface for
// the complete Begin/End scope, including any native submenus it opens.
inline void push_popup_style() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      {ui_pixels(12), ui_pixels(10)});
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                      {ui_pixels(8), ui_pixels(6)});
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      {ui_pixels(10), ui_pixels(6)});
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, ui_pixels(6));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, ui_pixels(1));
  // Modal popups use WindowRounding/WindowBorderSize rather than Popup*.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_pixels(6));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, ui_pixels(1));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, {0.110f, 0.141f, 0.176f, 1});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.110f, 0.141f, 0.176f, 1});
  ImGui::PushStyleColor(ImGuiCol_Border, {0.173f, 0.216f, 0.263f, 1});
  ImGui::PushStyleColor(ImGuiCol_Header, {0.137f, 0.243f, 0.243f, 1});
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.125f, 0.169f, 0.212f, 1});
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0.180f, 0.290f, 0.290f, 1});
  ImGui::PushStyleColor(ImGuiCol_TitleBg, {0.110f, 0.141f, 0.176f, 1});
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, {0.110f, 0.141f, 0.176f, 1});
}

inline void pop_popup_style() {
  ImGui::PopStyleColor(detail::popup_style_colors);
  ImGui::PopStyleVar(detail::popup_style_vars);
}

class PopupStyle final {
public:
  PopupStyle() { push_popup_style(); }
  ~PopupStyle() { pop_popup_style(); }
  PopupStyle(const PopupStyle &) = delete;
  PopupStyle &operator=(const PopupStyle &) = delete;
  PopupStyle(PopupStyle &&) = delete;
  PopupStyle &operator=(PopupStyle &&) = delete;
};

// Open from the control edge for mouse and keyboard activation alike. Upper
// toolbar menus drop below their owner; lower controls open upward.
inline void anchor_popup_to_item() {
  const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
  const auto *viewport = ImGui::GetMainViewport();
  const bool right = max.x > viewport->WorkPos.x + viewport->WorkSize.x * .65f;
  const bool above = min.y > viewport->WorkPos.y + viewport->WorkSize.y * .65f;
  ImGui::SetNextWindowPos(
      {right ? max.x : min.x, above ? min.y - ui_pixels(6) : max.y + ui_pixels(6)},
      ImGuiCond_Appearing, {right ? 1.f : 0.f, above ? 1.f : 0.f});
}

// Match Dear ImGui's conditional Begin contract: failed opens restore style
// immediately; successful opens retain it until the corresponding ui::End.
// Do not mix these Begin helpers with ImGui::EndPopup/EndCombo.
inline bool begin_popup(const char *id, ImGuiWindowFlags flags = 0) {
  push_popup_style();
  if (ImGui::BeginPopup(id, flags))
    return true;
  pop_popup_style();
  return false;
}

inline bool begin_popup_modal(const char *name, bool *open = nullptr,
                              ImGuiWindowFlags flags = 0) {
  push_popup_style();
  if (ImGui::BeginPopupModal(name, open, flags))
    return true;
  pop_popup_style();
  return false;
}

inline bool begin_popup_context_item(
    const char *id = nullptr,
    ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonRight) {
  push_popup_style();
  if (ImGui::BeginPopupContextItem(id, flags))
    return true;
  pop_popup_style();
  return false;
}

inline void end_popup() {
  ImGui::EndPopup();
  pop_popup_style();
}

// This also gives the source combo a standard 10x6dp FramePadding. Keep the
// caller's FrameBg (including transparent toolbar previews), widget ID and
// native keyboard/navigation path. ImGui aligns the popup's horizontal padding
// to FramePadding.x itself; all dropdowns therefore share that same inset.
inline bool begin_combo(const char *label, const char *preview,
                         ImGuiComboFlags flags = 0) {
  push_popup_style();
  if (ImGui::BeginCombo(label, preview, flags))
    return true;
  pop_popup_style();
  return false;
}

inline void end_combo() {
  ImGui::EndCombo();
  pop_popup_style();
}

enum class Icon {
  cube,
  file,
  chevron_down,
  chevron_left,
  chevron_right,
  first,
  last,
  play,
  pause,
  panel_left,
  panel_right,
  focus,
  sliders,
  chart,
  more,
  close,
  settings
};

// Icons use a shared 16-unit grid and are rasterized at the current physical
// size. They need no font glyphs, textures or renderer-specific resources.
inline void icon(Icon shape, ImVec2 origin, float size, ImU32 color,
                 ImDrawList *target = nullptr) {
  if (!(size > 0))
    return;
  auto *draw = target ? target : ImGui::GetWindowDrawList();
  const float scale = size / 16.f;
  const float stroke = std::max(.8f, 1.4f * scale);
  const auto point = [&](float x, float y) {
    return ImVec2{origin.x + x * scale, origin.y + y * scale};
  };
  const auto line = [&](float x1, float y1, float x2, float y2) {
    draw->AddLine(point(x1, y1), point(x2, y2), color, stroke);
  };
  const auto path = [&](std::initializer_list<ImVec2> points,
                        bool closed = false) {
    for (const auto &p : points)
      draw->PathLineTo(p);
    draw->PathStroke(color, closed ? ImDrawFlags_Closed : ImDrawFlags_None,
                     stroke);
  };
  switch (shape) {
  case Icon::cube:
    path({point(8, 1.5f), point(14, 5), point(14, 11.5f), point(8, 15),
          point(2, 11.5f), point(2, 5)}, true);
    path({point(2, 5), point(8, 8.5f), point(14, 5)});
    line(8, 8.5f, 8, 15);
    break;
  case Icon::file:
    path({point(4, 1.5f), point(10, 1.5f), point(13, 4.5f),
          point(13, 14.5f), point(4, 14.5f)}, true);
    path({point(10, 1.5f), point(10, 4.5f), point(13, 4.5f)});
    line(6, 8, 10, 8);
    line(6, 11, 9, 11);
    break;
  case Icon::chevron_down:
    path({point(4, 6), point(8, 10), point(12, 6)});
    break;
  case Icon::chevron_left:
    path({point(10, 4), point(6, 8), point(10, 12)});
    break;
  case Icon::chevron_right:
    path({point(6, 4), point(10, 8), point(6, 12)});
    break;
  case Icon::first:
    line(3, 3, 3, 13);
    path({point(12.5f, 3), point(5.5f, 8), point(12.5f, 13)}, true);
    break;
  case Icon::last:
    line(13, 3, 13, 13);
    path({point(3.5f, 3), point(10.5f, 8), point(3.5f, 13)}, true);
    break;
  case Icon::play:
    path({point(5, 2.5f), point(12.5f, 8), point(5, 13.5f)}, true);
    break;
  case Icon::pause:
    draw->AddRect(point(4, 3), point(6, 13), color, .5f * scale,
                   ImDrawFlags_None, stroke);
    draw->AddRect(point(10, 3), point(12, 13), color, .5f * scale,
                   ImDrawFlags_None, stroke);
    break;
  case Icon::panel_left:
  case Icon::panel_right:
    draw->AddRect(point(1.5f, 3), point(14.5f, 13), color, 1.f * scale,
                   ImDrawFlags_None, stroke);
    if (shape == Icon::panel_left)
      line(5.5f, 3, 5.5f, 13);
    else
      line(10.5f, 3, 10.5f, 13);
    break;
  case Icon::focus:
    path({point(2, 6), point(2, 2), point(6, 2)});
    path({point(10, 2), point(14, 2), point(14, 6)});
    path({point(14, 10), point(14, 14), point(10, 14)});
    path({point(6, 14), point(2, 14), point(2, 10)});
    draw->AddCircle(point(8, 8), 1.7f * scale, color, 0, stroke);
    break;
  case Icon::sliders:
    line(2, 4, 14, 4);
    line(2, 8, 14, 8);
    line(2, 12, 14, 12);
    line(5, 2, 5, 6);
    line(11, 6, 11, 10);
    line(7, 10, 7, 14);
    break;
  case Icon::chart:
    path({point(2, 2), point(2, 14), point(14, 14)});
    path({point(5, 10), point(8, 6), point(10.5f, 8), point(14, 3)});
    break;
  case Icon::more:
    draw->AddCircleFilled(point(3, 8), scale, color);
    draw->AddCircleFilled(point(8, 8), scale, color);
    draw->AddCircleFilled(point(13, 8), scale, color);
    break;
  case Icon::close:
    line(4, 4, 12, 12);
    line(12, 4, 4, 12);
    break;
  case Icon::settings:
    draw->AddCircle(point(8, 8), 3.5f * scale, color, 0, stroke);
    draw->AddCircle(point(8, 8), 1.f * scale, color, 0, stroke);
    for (int i = 0; i < 8; ++i) {
      const float angle = static_cast<float>(i) * .7853981634f;
      const float x = std::cos(angle), y = std::sin(angle);
      line(8 + x * 3.8f, 8 + y * 3.8f,
            8 + x * 6.f, 8 + y * 6.f);
    }
    break;
  }
}

namespace detail {
inline bool button(const char *id, ImVec2 size, bool selected) {
  const auto &style = ImGui::GetStyle();
  ImGui::PushStyleColor(ImGuiCol_Button,
      selected ? style.Colors[ImGuiCol_Header] : ImVec4{0, 0, 0, 0});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
      selected ? style.Colors[ImGuiCol_HeaderHovered]
               : style.Colors[ImGuiCol_FrameBgHovered]);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, style.Colors[ImGuiCol_HeaderActive]);
  // Preserve the caller's exact widget ID and native Button navigation/input.
  // Its visible label is drawn below, together with the icon.
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0, 0, 0, 0});
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
  const bool pressed = ImGui::Button(id, size);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  return pressed;
}
inline void tooltip(const char *label) {
  if (label && *label && ImGui::IsItemHovered(
          ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", label);
}
} // namespace detail

inline bool icon_button(const char *id, Icon shape, const char *tooltip,
                         bool selected = false,
                         float size_dp = control_height_dp) {
  const float side = ui_pixels(std::max(1.f, size_dp));
  const bool pressed = detail::button(id, {side, side}, selected);
  const auto min = ImGui::GetItemRectMin();
  const float glyph_size = std::max(1.f, std::min(ui_pixels(icon_size_dp),
                                                side - ui_pixels(6)));
  icon(shape, {min.x + (side - glyph_size) / 2,
                min.y + (side - glyph_size) / 2}, glyph_size,
         ImGui::GetColorU32(selected ? ImGuiCol_CheckMark : ImGuiCol_Text));
  detail::tooltip(tooltip);
  return pressed;
}

inline bool labeled_button(const char *id, Icon shape, const char *label,
                            bool selected = false,
                            float height_dp = control_height_dp) {
  if (!label)
    label = "";
  const float height = ui_pixels(std::max(1.f, height_dp));
  const float padding = ui_pixels(9), gap = ui_pixels(7);
  const float glyph_size = ui_pixels(icon_size_dp);
  const auto text_size = ImGui::CalcTextSize(label, nullptr, false);
  const float width = padding * 2 + glyph_size + gap + text_size.x;
  const bool pressed = detail::button(id, {width, height}, selected);
  const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
  const ImU32 color =
      ImGui::GetColorU32(selected ? ImGuiCol_CheckMark : ImGuiCol_Text);
  icon(shape, {min.x + padding, min.y + (height - glyph_size) / 2},
         glyph_size, color);
  const ImVec2 text_pos{min.x + padding + glyph_size + gap,
                         min.y + (height - text_size.y) / 2};
  const ImVec4 clip{min.x, min.y, max.x, max.y};
  ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
      text_pos, color, label, nullptr, 0.f, &clip);
  return pressed;
}

} // namespace pvd::viewer::ui
