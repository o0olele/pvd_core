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
// Returns true if the user changes the camera (not just navigation preferences).
bool drawCameraControls(const Document &, OrbitCamera &,
                         CameraNavigationPreferences &, CameraPanelState &);
// Actual ImGui input adapter. Keyboard navigation belongs to the hovered
// viewport only, never to a text editor, popup or Ctrl/Alt/Super shortcut.
bool navigateCameraFromImGui(OrbitCamera &, const CameraNavigationPreferences &,
                            bool viewport_hovered);
} // namespace pvd::viewer
