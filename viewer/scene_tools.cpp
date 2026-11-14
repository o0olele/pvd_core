#include "scene_tools.hpp"
#include "imgui.h"
#include <algorithm>
#include <stdexcept>

namespace pvd::viewer {
namespace {
template <class F> void action(SceneToolState &state, F &&fn) {
  try {
    fn();
    state.error.clear();
  } catch (const std::exception &e) {
    state.error = e.what();
  }
}
ImVec4 rgba(Color c) {
  return {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
}
} // namespace
void drawSceneTools(Document &doc, OrbitCamera &camera, SceneToolState &state) {
  if (!doc.loaded()) {
    ImGui::TextWrapped("Open a capture or connect live to inspect the scene.");
    return;
  }
  if (state.document_source != doc.session()) {
    stopTracking(state);
    state.recorded_camera.clear();
    state.error.clear();
    state.document_source = doc.session();
  }
  auto options = doc.view_options();
  bool changed = false;
  ImGui::TextDisabled("Visibility affects this viewer only.");
  changed |= ImGui::Checkbox("Static", &options.show_static);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Dynamic", &options.show_dynamic);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Kinematic", &options.show_kinematic);
  changed |= ImGui::Checkbox("Sleeping", &options.show_sleeping);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Trigger", &options.show_trigger);
  ImGui::TextWrapped(
      "Dynamic excludes kinematic; Sleeping and Trigger are extra filters.");
  const auto selected = doc.selected_key();
  ImGui::BeginDisabled(!selected);
  if (ImGui::Button("Hide selected") && selected)
    action(state, [&] { doc.set_visible(*selected, false); });
  ImGui::SameLine();
  if (ImGui::Button("Isolate selected") && selected)
    action(state, [&] { doc.isolate(*selected); });
  ImGui::EndDisabled();
  if (ImGui::Button("Restore all"))
    action(state, [&] {
      doc.show_all();
      options = doc.view_options();
      changed = false;
    });
  if (doc.isolated()) {
    ImGui::SameLine();
    ImGui::TextDisabled("Isolated view");
  }
  std::size_t visible = 0;
  for (const auto &d : doc.frame()->draws)
    visible += d.visible;
  ImGui::SameLine();
  ImGui::TextDisabled("%zu / %zu shapes visible", visible,
                      doc.frame()->draws.size());
  if (changed)
    action(state, [&] { doc.set_view_options(options); });
  ImGui::Separator();
  int mode = static_cast<int>(options.color_mode);
  const char *modes[] = {"Recorded / status", "Actor kind", "Geometry kind",
                         "Actor lifetime", "Uniform"};
  ImGui::SetNextItemWidth(std::min(220.f, ImGui::GetContentRegionAvail().x));
  if (ImGui::Combo("Color mode", &mode, modes, 5))
    action(state, [&] {
      options.color_mode = static_cast<ColorMode>(mode);
      doc.set_view_options(options);
    });
  if (options.color_mode == ColorMode::uniform) {
    auto c = rgba(options.uniform_color);
    float values[] = {c.x, c.y, c.z, c.w};
    if (ImGui::ColorEdit4("Uniform color", values,
                          ImGuiColorEditFlags_NoInputs))
      action(state, [&] {
        options.uniform_color = {
            static_cast<std::uint8_t>(std::clamp(values[0], 0.f, 1.f) * 255 +
                                      .5f),
            static_cast<std::uint8_t>(std::clamp(values[1], 0.f, 1.f) * 255 +
                                      .5f),
            static_cast<std::uint8_t>(std::clamp(values[2], 0.f, 1.f) * 255 +
                                      .5f),
            static_cast<std::uint8_t>(std::clamp(values[3], 0.f, 1.f) * 255 +
                                      .5f)};
        doc.set_view_options(options);
      });
  }
  ImGui::PushID("color-legend");
  for (const auto &entry : view_legend(options)) {
    ImGui::ColorButton(entry.label.c_str(), rgba(entry.color),
                       ImGuiColorEditFlags_NoTooltip, {12, 12});
    ImGui::SameLine();
    ImGui::TextUnformatted(entry.label.c_str());
  }
  ImGui::PopID();
  if (options.color_mode == ColorMode::recorded_status)
    ImGui::TextWrapped(
        "Recorded per-object colors take priority. Remaining shapes use "
        "Trigger, Kinematic, Static, Sleeping, Dynamic in that order.");
  if (options.color_mode == ColorMode::actor_identity)
    ImGui::TextWrapped("Stable color per scene and actor generation; shared "
                       "shape attachments may differ.");
  ImGui::Separator();
  if (ImGui::Button("Fit visible"))
    action(state, [&] {
      if (!doc.fit_scene(camera, state.viewport_aspect))
        throw std::runtime_error("No visible scene bounds");
      stopTracking(state);
    });
  ImGui::SameLine();
  if (ImGui::Button("Fit selected"))
    action(state, [&] {
      if (!doc.fit_selected(camera, state.viewport_aspect))
        throw std::runtime_error("Selected object has no visible geometry");
      stopTracking(state);
    });
  ImGui::BeginDisabled(!doc.inspected_object() && !selected);
  if (ImGui::Button("Track selected"))
    action(state, [&] {
      if (!trackSelected(doc, camera, state))
        throw std::runtime_error(state.message);
    });
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!state.tracking());
  if (ImGui::Button("Stop tracking"))
    stopTracking(state);
  ImGui::EndDisabled();
  if (state.tracking())
    ImGui::TextWrapped(
        "Tracking active; orbit, zoom and pan offsets are retained.");
  ImGui::TextWrapped("%s", state.message.c_str());
  ImGui::SetNextItemWidth(std::min(220.f, ImGui::GetContentRegionAvail().x));
  if (ImGui::BeginCombo("Recorded camera",
                        state.recorded_camera.empty()
                            ? "Choose captured camera"
                            : state.recorded_camera.c_str())) {
    for (const auto &entry : doc.frame()->snapshot.cameras)
      if (ImGui::Selectable(entry.first.c_str(),
                            state.recorded_camera == entry.first))
        action(state, [&] {
          if (!applyRecordedCamera(doc, entry.first, camera, state))
            throw std::runtime_error(state.message);
        });
    ImGui::EndCombo();
  }
  if (doc.frame()->snapshot.cameras.empty())
    ImGui::TextDisabled("No camera state captured at this frame.");
  ImGui::TextWrapped("Recorded camera up vectors are preserved; orbit uses the "
                     "chosen camera's up axis.");
  if (!state.error.empty())
    ImGui::TextWrapped("Scene tools: %s", state.error.c_str());
}
} // namespace pvd::viewer
