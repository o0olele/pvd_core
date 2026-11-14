#include "display_panel.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include <stdexcept>

namespace pvd::viewer {
namespace {
template <class F> void action(DisplayPanelState &state, F &&fn) {
  try {
    fn();
    state.error.clear();
  } catch (const std::exception &e) {
    state.error = e.what();
  }
}
} // namespace
void drawDisplaySettings(Document &doc, OrbitCamera &camera,
                         RenderOptions &options, const RenderStats &stats,
                         DisplayPanelState &state) {
  ImGui::SetNextItemWidth(ui_pixels(180));
  ImGui::ColorEdit3("Background", options.background_color.data(),
                    ImGuiColorEditFlags_NoInputs);
  ImGui::SetNextItemWidth(ui_pixels(180));
  int aa = options.antialiasing == Antialiasing::msaa4x ? 1 : 0;
  if (ImGui::Combo("Antialiasing", &aa, "Off (1x)\0MSAA 4x\0"))
    options.antialiasing = aa ? Antialiasing::msaa4x : Antialiasing::off;
  const auto requested = options.antialiasing == Antialiasing::msaa4x ? 4u : 1u;
  const auto &actual = stats.multisampling;
  if (requested != actual.requested_samples)
    ImGui::TextWrapped(
        "Requested %ux; awaiting viewport render (last actual %ux).", requested,
        actual.actual_samples);
  else if (actual.fallback)
    ImGui::TextWrapped("Requested 4x; actual 1x. Device lacks compatible 4x "
                       "color/depth/resolve support.");
  else
    ImGui::Text("Requested %ux; actual %ux", actual.requested_samples,
                actual.actual_samples);
  ImGui::SetNextItemWidth(ui_pixels(180));
  const auto axis = display_up(camera);
  const char *up_names[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
  if (ImGui::BeginCombo("Up axis", axis ? up_names[static_cast<int>(*axis)]
                                        : "Recorded/custom")) {
    for (int up = 0; up < 6; ++up)
      if (ImGui::Selectable(up_names[up],
                            axis && static_cast<int>(*axis) == up))
        action(state,
               [&] { set_display_up(camera, static_cast<DisplayUp>(up)); });
    ImGui::EndCombo();
  }
  if (!axis)
    ImGui::TextWrapped(
        "Recorded/custom up: %.4g, %.4g, %.4g", double(camera.reference_up.x),
        double(camera.reference_up.y), double(camera.reference_up.z));
  ImGui::SetNextItemWidth(ui_pixels(180));
  int hand = camera.handedness == DisplayHandedness::left_handed ? 1 : 0;
  if (ImGui::Combo("Display handedness", &hand,
                   "Right-handed\0Left-handed (mirror)\0"))
    camera.handedness =
        hand ? DisplayHandedness::left_handed : DisplayHandedness::right_handed;
  ImGui::TextWrapped(
      "Up controls the orbit/grid plane. Left-handed display mirrors the view "
      "horizontally; picking and camera controls follow it. Source physics "
      "coordinates, world poses and exports stay unchanged.");
  ImGui::Checkbox("Joint frames", &options.show_joint_frames);
  ImGui::SameLine();
  ImGui::Checkbox("Joint limits", &options.show_joint_limits);
  ImGui::Checkbox("Tight geometry bounds", &options.show_bounds);
  ImGui::Checkbox("Linear velocity", &options.show_linear_velocity);
  ImGui::SameLine();
  ImGui::Checkbox("Angular velocity", &options.show_angular_velocity);
  if (options.show_linear_velocity || options.show_angular_velocity) {
    ImGui::SetNextItemWidth(ui_pixels(180));
    ImGui::DragFloat("Rigid velocity scale", &options.rigid_velocity_scale,
                     .01f, 0.f, 1e6f, "%.3g", ImGuiSliderFlags_AlwaysClamp);
  }
  ImGui::TextWrapped("Bounds use tight captured geometry, not broadphase inflation. "
                     "Velocity arrows use recorded world-space values at the actor pose.");
  ImGui::BeginDisabled(!doc.loaded());
  if (ImGui::Button("Fit scene with this display"))
    action(state, [&] {
      if (!doc.fit_scene(camera, state.viewport_aspect))
        throw std::runtime_error(
            "This sample has no captured visible world bounds to fit");
    });
  ImGui::EndDisabled();
  if (!state.error.empty())
    ImGui::TextWrapped("%s", state.error.c_str());
}
} // namespace pvd::viewer
