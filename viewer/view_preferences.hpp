#pragma once
#include "inspection_preferences.hpp"
#include "renderer.hpp"
#include <cmath>

namespace pvd::viewer {
struct ViewPreferences {
  RenderOptions rendering;
  InspectorPreferences inspection;
  bool show_debug_text = true;
};
// Same input constraints as Renderer plus the inspector's independent policy.
// A preference value is UI-free and does not initialize a GPU or native window.
inline bool valid_view_preferences(const ViewPreferences &v) noexcept {
  const auto &r = v.rendering;
  if ((r.antialiasing != Antialiasing::off &&
       r.antialiasing != Antialiasing::msaa4x) ||
      !valid_inspector_preferences(v.inspection))
    return false;
  for (float component : r.background_color)
    if (!std::isfinite(component) || component < 0 || component > 1)
      return false;
  const auto scale = [](float n) {
    return std::isfinite(n) && n >= 0 && n <= 1e6f;
  };
  return std::isfinite(r.point_size) && r.point_size >= 1 &&
         r.point_size <= 64 && std::isfinite(r.contact_normal_length) &&
         r.contact_normal_length >= 0 && scale(r.particle_velocity_scale) &&
         scale(r.rigid_velocity_scale);
}
} // namespace pvd::viewer
