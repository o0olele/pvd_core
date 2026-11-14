#pragma once
#include "camera_controller.hpp"
#include "inspection_preferences.hpp"
#include "renderer.hpp"
#include <cmath>

namespace pvd::viewer {
struct ViewPreferences {
  RenderOptions rendering;
  InspectorPreferences inspection;
  bool show_debug_text = true;
  CameraNavigationPreferences navigation;
};
// Same input constraints as Renderer plus the inspector's independent policy.
// A preference value is UI-free and does not initialize a GPU or native window.
inline bool valid_view_preferences(const ViewPreferences &v) noexcept {
  const auto &r = v.rendering;
  if ((r.antialiasing != Antialiasing::off &&
       r.antialiasing != Antialiasing::msaa4x) ||
      (r.face_culling != FaceCulling::none &&
       r.face_culling != FaceCulling::front &&
       r.face_culling != FaceCulling::back) ||
      !valid_inspector_preferences(v.inspection) ||
      !valid_camera_navigation(v.navigation))
    return false;
  for (float component : r.background_color)
    if (!std::isfinite(component) || component < 0 || component > 1)
      return false;
  const auto scale = [](float n) {
    return std::isfinite(n) && n >= 0 && n <= 1e6f;
  };
  const auto scope = [](VisualizationScope n) {
    return n >= VisualizationScope::none && n <= VisualizationScope::unselected;
  };
  if (!scope(r.bounds_scope) || !scope(r.velocity_scope) ||
      !scope(r.scene_query_scope) || !scope(r.coordinate_frame_scope) ||
      !scope(r.transparent_scope) || !scale(r.coordinate_frame_scale) ||
      !std::isfinite(r.transparent_opacity) || r.transparent_opacity < 0 ||
      r.transparent_opacity > 1)
    return false;
  return std::isfinite(r.point_size) && r.point_size >= 1 &&
         r.point_size <= 64 && std::isfinite(r.contact_normal_length) &&
         r.contact_normal_length >= 0 && scale(r.particle_velocity_scale) &&
         scale(r.rigid_velocity_scale);
}
} // namespace pvd::viewer
