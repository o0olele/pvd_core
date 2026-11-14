#include "scene_tools.hpp"
#include "palette_editor.hpp"
#include "ui_scale.hpp"
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
SceneToolRect item_rect() {
  const auto p=ImGui::GetItemRectMin(), size=ImGui::GetItemRectSize();
  return {p.x,p.y,size.x,size.y};
}
} // namespace
void drawSceneTools(Document &doc, OrbitCamera &camera, SceneToolState &state) {
  if (!doc.loaded()) {
    ImGui::TextWrapped("Open a capture or connect live to inspect the scene.");
    return;
  }
  if (state.document_source != doc.session()) {
    const bool camera_source_changed = state.recorded_follow.enabled ||
        state.recorded_follow.status == RecordedCameraStatus::source_changed;
    stopTracking(state);
    state.recorded_camera.clear();
    state.error.clear();
    state.document_source = doc.session();
    if (camera_source_changed) {
      state.recorded_follow.status=RecordedCameraStatus::source_changed;
      state.recorded_follow.diagnostic="Recorded camera follow stopped: capture/source changed";
      state.message=state.recorded_follow.diagnostic;
    }
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
  const char *scopes[] = {"None", "All", "Selected", "Unselected"};
  int sleeping_scope = static_cast<int>(options.sleeping_scope);
  ImGui::SetNextItemWidth(std::min(ui_pixels(140.f),
                                  ImGui::GetContentRegionAvail().x));
  ImGui::BeginDisabled(!options.show_sleeping);
  if (ImGui::Combo("Sleeping scope", &sleeping_scope, scopes, 4)) {
    options.sleeping_scope = static_cast<VisualizationScope>(sleeping_scope);
    changed = true;
  }
  ImGui::EndDisabled();
  changed |= ImGui::Checkbox("Trigger", &options.show_trigger);
  int trigger_scope = static_cast<int>(options.trigger_scope);
  ImGui::SetNextItemWidth(std::min(ui_pixels(140.f),
                                  ImGui::GetContentRegionAvail().x));
  ImGui::BeginDisabled(!options.show_trigger);
  if (ImGui::Combo("Trigger scope", &trigger_scope, scopes, 4)) {
    options.trigger_scope = static_cast<VisualizationScope>(trigger_scope);
    changed = true;
  }
  ImGui::EndDisabled();
  ImGui::TextWrapped(
      "Dynamic excludes kinematic. Sleeping/Trigger scopes update with the "
      "current inspected owner or selected attachment; awake and non-trigger "
      "shapes keep their category visibility. Hide/isolate still apply.");
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
  ImGui::SetNextItemWidth(std::min(ui_pixels(220.f), ImGui::GetContentRegionAvail().x));
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
  if (ImGui::CollapsingHeader("Object palette")) {
    const auto edited = draw_object_palette_editor(options.palette);
    if (!edited.error.empty())
      state.error = edited.error;
    if (edited.changed)
      action(state, [&] { doc.set_view_options(options); });
  }
  ImGui::PushID("color-legend");
  for (const auto &entry : view_legend(options)) {
    ImGui::ColorButton(entry.label.c_str(), rgba(entry.color),
                       ImGuiColorEditFlags_NoTooltip, {ui_pixels(12), ui_pixels(12)});
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
  if (state.tracking_object())
    ImGui::TextWrapped(
        "Tracking active; orbit, zoom and pan offsets are retained.");
  ImGui::TextWrapped("%s", state.message.c_str());
  ImGui::SetNextItemWidth(std::min(ui_pixels(220.f), ImGui::GetContentRegionAvail().x));
  const auto combo_position=ImGui::GetCursorScreenPos();
  state.recorded_combo_rect={combo_position.x,combo_position.y,
                            ImGui::CalcItemWidth(),ImGui::GetFrameHeight()};
  state.recorded_choice_rects.clear();
  if (ImGui::BeginCombo("Recorded camera",
                        state.recorded_camera.empty()
                            ? "Choose captured camera"
                            : state.recorded_camera.c_str())) {
    const auto frame = doc.frame();
    for (const auto &entry : frame->snapshot.cameras) {
      const auto chosen=ImGui::Selectable(entry.first.c_str(),
                            state.recorded_camera == entry.first);
      state.recorded_choice_rects[entry.first]=item_rect();
      if (chosen)
        action(state, [&] {
          if (!followRecordedCamera(doc, entry.first, camera, state))
            throw std::runtime_error(state.message);
        });
    }
    ImGui::EndCombo();
  }
  ImGui::BeginDisabled(state.recorded_camera.empty());
  if (ImGui::Button("Follow recorded"))
    action(state, [&] {
      if (!followRecordedCamera(doc, state.recorded_camera, camera, state))
        throw std::runtime_error(state.message);
    });
  ImGui::SameLine();
  const auto apply_once=ImGui::Button("Apply once");
  state.apply_camera_rect=item_rect();
  if (apply_once)
    action(state, [&] {
      if (!applyRecordedCamera(doc, state.recorded_camera, camera, state))
        throw std::runtime_error(state.message);
    });
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!state.recorded_follow.enabled);
  const auto free_camera=ImGui::Button("Free camera");
  state.free_camera_rect=item_rect();
  if (free_camera)
    releaseRecordedCamera(state);
  ImGui::EndDisabled();
  if (state.recorded_follow.enabled)
    ImGui::TextWrapped("Clip camera follows the exact named snapshot on seek "
                       "and playback. Manual camera input releases this mode; "
                       "projection settings stay under your control.");
  if (doc.frame()->snapshot.cameras.empty())
    ImGui::TextDisabled("No camera state captured at this frame.");
  ImGui::TextWrapped("Recorded camera up vectors are preserved; orbit uses the "
                     "chosen camera's up axis.");
  if (!state.error.empty())
    ImGui::TextWrapped("Scene tools: %s", state.error.c_str());
}
} // namespace pvd::viewer
