#pragma once
#include "camera_controller.hpp"
#include <string>
namespace pvd::viewer {
struct CameraPanelState {
  const Session *source = nullptr;
  Vec3 target{};
  bool initialized = false, target_dirty = false;
  std::string error;
};
struct CameraInputState {
  CameraMovementState movement;
  int last_frame = -1;
};
// Returns true if the user changes the camera (not just navigation preferences).
bool drawCameraControls(const Document &, OrbitCamera &,
                         CameraNavigationPreferences &, CameraPanelState &);
// Actual ImGui input adapter. Keyboard navigation belongs to the hovered
// viewport only, never to a text editor, popup or Ctrl/Alt/Super shortcut.
bool navigateCameraFromImGui(OrbitCamera &, const CameraNavigationPreferences &,
                            CameraInputState &, bool viewport_hovered);
// The host supplies active canvas ownership and physical/logical pixel scale.
bool rotateCameraFromImGui(OrbitCamera &, const CameraNavigationPreferences &,
                          bool viewport_active, float screen_scale);
} // namespace pvd::viewer
