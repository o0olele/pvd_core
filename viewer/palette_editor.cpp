#include "palette_editor.hpp"
#include "imgui.h"
#include "ui_scale.hpp"

namespace pvd::viewer {
namespace {
PaletteControlRect item_rect() {
  const auto p = ImGui::GetItemRectMin(), s = ImGui::GetItemRectSize();
  return {p.x, p.y, s.x, s.y};
}
bool same(const ColorPalette &a, const ColorPalette &b) {
  for (auto role : color_palette_roles)
    if (palette_color(a, role) != palette_color(b, role))
      return false;
  return a.profile_by_event == b.profile_by_event;
}
} // namespace
PaletteEditResult draw_object_palette_editor(ColorPalette &palette) {
  PaletteEditResult result;
  if (!valid_color_palette(palette)) {
    result.error = "Palette RGBA components must be finite and in [0,1]";
    ImGui::TextWrapped("%s", result.error.c_str());
    return result;
  }
  auto candidate = palette;
  if (ImGui::BeginTable("Object palette", 2,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch,
                            1.f);
    ImGui::TableSetupColumn("RGBA", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    for (auto role : color_palette_roles) {
      if (role == PaletteRole::profile_bar)
        continue;
      ImGui::PushID(static_cast<int>(role));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(palette_role_name(role));
      if (role == PaletteRole::articulation && ImGui::IsItemHovered())
        ImGui::SetTooltip("Actor kind mode uses this color. Recorded/status "
                          "retains sleep/active status colors.");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      ImGui::ColorEdit4("##RGBA", palette_color(candidate, role).data(),
                        ImGuiColorEditFlags_Float |
                            ImGuiColorEditFlags_AlphaPreviewHalf |
                            ImGuiColorEditFlags_AlphaBar);
      result.colors[static_cast<std::size_t>(role)] = item_rect();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  const auto restore = ImGui::Button("Restore object palette");
  result.restore = item_rect();
  if (restore) {
    const auto defaults = default_color_palette();
    for (auto role : color_palette_roles)
      if (role != PaletteRole::profile_bar)
        palette_color(candidate, role) = palette_color(defaults, role);
    result.restored = true;
  }
  ImGui::TextWrapped(
      "Recorded colors remain first in Recorded/status and native "
      "cloth/particle views. Explicit color modes override status. These edits "
      "change display only; profile bar colors have separate controls.");
  if (!valid_color_palette(candidate))
    result.error =
        "Invalid palette edit rejected; RGBA must be finite and in [0,1]";
  else if (!same(candidate, palette)) {
    palette = candidate;
    result.changed = true;
  }
  return result;
}
} // namespace pvd::viewer
