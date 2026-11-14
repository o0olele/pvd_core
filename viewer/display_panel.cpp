#include "display_panel.hpp"
#include "imgui.h"
#include "ui_scale.hpp"
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
void scope(const char *label, VisualizationScope &value) {
  int selected = static_cast<int>(value);
  ImGui::SetNextItemWidth(ui_pixels(180));
  if (ImGui::Combo(label, &selected, "None\0All\0Selected\0Unselected\0"))
    value = static_cast<VisualizationScope>(selected);
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
  scope("Bounds scope", options.bounds_scope);
  ImGui::Checkbox("Linear velocity", &options.show_linear_velocity);
  ImGui::SameLine();
  ImGui::Checkbox("Angular velocity", &options.show_angular_velocity);
  scope("Velocity scope", options.velocity_scope);
  if (options.show_linear_velocity || options.show_angular_velocity) {
    ImGui::SetNextItemWidth(ui_pixels(180));
    ImGui::DragFloat("Rigid velocity scale", &options.rigid_velocity_scale,
                     .01f, 0.f, 1e6f, "%.3g", ImGuiSliderFlags_AlwaysClamp);
  }
  ImGui::TextWrapped(
      "Bounds use tight captured geometry, not broadphase inflation. "
      "Velocity arrows use recorded world-space values at the actor pose.");
  scope("Scene query scope", options.scene_query_scope);
  ImGui::TextWrapped(
      "Selected query scope uses the exact selected query row; query "
      "visibility, normals and volumes are also controlled in Scene queries.");
  ImGui::Separator();
  ImGui::Checkbox("Actor coordinate frames", &options.show_actor_frames);
  ImGui::Checkbox("Shape coordinate frames", &options.show_shape_frames);
  ImGui::Checkbox("Mass coordinate frames", &options.show_mass_frames);
  scope("Coordinate frame scope", options.coordinate_frame_scope);
  ImGui::SetNextItemWidth(ui_pixels(180));
  ImGui::DragFloat("Coordinate frame scale", &options.coordinate_frame_scale,
                   .01f, 0, 1e6f, "%.3g", ImGuiSliderFlags_AlwaysClamp);
  ImGui::TextWrapped("Axes are local X/red, Y/green, Z/blue in current native "
                     "world coordinates. Mass frames use Actor.GlobalPose * "
                     "CMassLocalPose for dynamic bodies and articulation "
                     "links; missing captured poses remain diagnostics.");
  if (!stats.coordinate_frames_complete)
    ImGui::TextWrapped(
        "Coordinate frames incomplete: %zu diagnostic(s). Requested missing or "
        "invalid frame data was not fabricated.",
        stats.coordinate_frame_diagnostics);
  scope("Transparent scope", options.transparent_scope);
  ImGui::SetNextItemWidth(ui_pixels(180));
  ImGui::SliderFloat("Opacity", &options.transparent_opacity, 0, 1, "%.2f");
  ImGui::TextWrapped(
      "Transparent shapes are depth-tested and composited back-to-front "
      "without writing depth. Zero opacity also excludes those attachments "
      "from picking. Explicit actor/articulation/aggregate inspection scopes "
      "its recorded owner graph; a shared shape selection stays "
      "attachment-specific.");
  ImGui::SetNextItemWidth(ui_pixels(180));
  ImGui::SliderFloat("Point size (pixels)", &options.point_size, 1, 64, "%.1f");
  ImGui::SetNextItemWidth(ui_pixels(180));
  ImGui::DragFloat("Contact normal length", &options.contact_normal_length,
                   .01f, 0, 1e6f, "%.3g", ImGuiSliderFlags_AlwaysClamp);
  ImGui::TextWrapped(
      "Point size controls native particles, cloth vertices and debug/contact "
      "markers. Contact normal length changes display geometry only; recorded "
      "positions and normals remain unchanged.");
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
