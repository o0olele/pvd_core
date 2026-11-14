#include "imgui.h"
#include "imgui_internal.h"
#include "palette_editor.hpp"
#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using namespace pvd::viewer;
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
struct Output {
  PaletteEditResult edit;
  float overflow = 0;
};
Output draw(ColorPalette &p, float width, std::optional<ImVec2> mouse = {},
            std::optional<bool> down = {}) {
  auto &io = ImGui::GetIO();
  io.DisplaySize = {width, 900};
  io.DeltaTime = 1.f / 60;
  if (mouse)
    io.AddMousePosEvent(mouse->x, mouse->y);
  if (down)
    io.AddMouseButtonEvent(0, *down);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize({width, 850});
  ImGui::Begin("Palette test", nullptr,
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoTitleBar |
                   ImGuiWindowFlags_NoSavedSettings);
  Output result;
  result.edit = draw_object_palette_editor(p);
  const auto *w = ImGui::GetCurrentWindow();
  result.overflow = std::max(0.f, w->DC.CursorMaxPos.x - w->InnerRect.Max.x);
  ImGui::End();
  ImGui::Render();
  return result;
}
void widgets(float width) {
  auto p = default_color_palette();
  p.profile_by_event = false;
  p.profile_bar = {.1f, .2f, .3f, .4f};
  const auto old = p.static_actor, profile = p.profile_bar;
  draw(p, width);
  const auto initial = draw(p, width);
  const auto r =
      initial.edit.colors[static_cast<std::size_t>(PaletteRole::static_actor)];
  check(r.width > 50 && r.height > 0, "actual RGBA widget rect missing");
  const ImVec2 start{r.x + 10, r.y + r.height * .5f},
      move{start.x + 40, start.y};
  draw(p, width, start, false);
  draw(p, width, start, true);
  const auto changed = draw(p, width, move, true);
  const auto changed_more = draw(p, width, ImVec2{move.x + 20, move.y}, true);
  draw(p, width, ImVec2{move.x + 20, move.y}, false);
  check(p.static_actor != old &&
            (changed.edit.changed || changed_more.edit.changed),
        "actual RGBA mouse drag did not update palette");
  check(
      valid_color_palette(p) && p.profile_bar == profile && !p.profile_by_event,
      "object color editing altered profile choice or accepted invalid values");
  auto output = draw(p, width);
  const auto reset = output.edit.restore;
  const ImVec2 at{reset.x + reset.width * .5f, reset.y + reset.height * .5f};
  draw(p, width, at, false);
  draw(p, width, at, true);
  output = draw(p, width, at, false);
  check(output.edit.restored && p.static_actor == old,
        "actual restore button did not restore object defaults");
  check(p.profile_bar == profile && !p.profile_by_event,
        "object restore reset profile preferences");
  check(initial.overflow <= 1 && output.overflow <= 1,
        "palette controls overflow available width");
  std::cout << "palette widgets width=" << width
            << " RGBA mouse edit/restore/profile separation passed\n";
}
void legacy_profile_oracle() {
  const auto p = default_color_palette();
  for (std::uint32_t id = 0; id <= 65535; ++id) {
    const auto color = profile_bar_color(static_cast<std::uint16_t>(id), p);
    const auto expected =
        ImColor::HSV(float((id * 37u) % 255u) / 255.f, .50f, .68f);
    check(ImGui::ColorConvertFloat4ToU32(
              {color[0], color[1], color[2], color[3]}) ==
              ImGui::ColorConvertFloat4ToU32(expected.Value),
          "default profile colors changed from original ImGui event HSV");
  }
}
} // namespace
int main() {
  try {
    legacy_profile_oracle();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char *pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    widgets(1060);
    widgets(420);
    ImGui::DestroyContext();
    std::cout << "palette editor and 65536 event-color oracle passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
