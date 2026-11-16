#pragma once
#include "model.hpp"
#include <cmath>

namespace pvd::viewer {
enum class CameraNavigationMode { orbit, first_person };
struct CameraNavigationPreferences {
  // Retained for workspace compatibility and explicit numerical rotations.
  // Scene gestures choose their own mode: RMB looks, Alt+LMB orbits.
  CameraNavigationMode mode = CameraNavigationMode::orbit;
  double movement_speed = 10; // World units/second; Shift multiplies by two.
  double look_sensitivity = .008; // Radians per logical mouse pixel.
};
inline bool valid_camera_navigation(const CameraNavigationPreferences &p) noexcept {
  return (p.mode == CameraNavigationMode::orbit ||
          p.mode == CameraNavigationMode::first_person) &&
         std::isfinite(p.movement_speed) && p.movement_speed >= 0 &&
         p.movement_speed <= 1e6 && std::isfinite(p.look_sensitivity) &&
         p.look_sensitivity >= 1e-6 && p.look_sensitivity <= .1;
}
struct CameraMovement {
  double forward = 0, right = 0, up = 0; // Each in [-1,1].
  double elapsed_seconds = 0; // Host bounds frame delta; accepted range [0,1].
  bool boost = false;
};
struct CameraMovementState {
  double held_seconds = 0; // Transient; resets when navigation stops.
};
// UI/OS independent. The host gates keyboard ownership and supplies logical
// mouse pixels. Movement uses recorded display-up, screen-right and view-forward;
// combined direction is normalized to avoid faster diagonal movement.
// All operations validate before publishing and leave source recording values
// untouched. Orbit rotation retains pivot; first-person rotation retains eye.
bool move_camera(OrbitCamera &, const CameraMovement &,
                 const CameraNavigationPreferences &);
// Continuous movement ramps from base speed to 8x over three seconds.
bool move_camera(OrbitCamera &, const CameraMovement &,
                 const CameraNavigationPreferences &, CameraMovementState &);
bool rotate_camera(OrbitCamera &, double logical_dx, double logical_dy,
                   const CameraNavigationPreferences &);
void point_camera_at(OrbitCamera &, Vec3 target);
void set_camera_projection(OrbitCamera &, double near_plane, double far_plane,
                           double vertical_fov_radians);
} // namespace pvd::viewer
