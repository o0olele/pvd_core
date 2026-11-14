#pragma once
#include "model.hpp"
#include "recorded_camera.hpp"
#include <map>

namespace pvd::viewer {
// Actual widget bounds, exposed without ImGui types for headless mouse tests.
struct SceneToolRect {
  float x = 0, y = 0, width = 0, height = 0;
};
// UI-independent camera tracking state. The host calls updateTrackedCamera once
// after Document::advance/seek and before rendering. Tracking translates the
// camera by the target's motion, retaining the user's orbit/zoom/pan offset.
struct SceneToolState {
  double viewport_aspect = 1.0;
  const Session *tracked_source = nullptr;
  const Session *document_source = nullptr;
  std::optional<PickingKey> tracked_shape;
  std::optional<ObjectKey> tracked_object;
  std::optional<Vec3> tracked_anchor;
  std::string recorded_camera, message, error;
  RecordedCameraFollowState recorded_follow;
  SceneToolRect recorded_combo_rect, free_camera_rect, apply_camera_rect;
  std::map<std::string, SceneToolRect> recorded_choice_rects;
  bool tracking_object() const noexcept {
    return tracked_shape.has_value() || tracked_object.has_value();
  }
  bool tracking() const noexcept {
    return tracking_object() || recorded_follow.enabled;
  }
};
// These helpers are implemented in the UI-free viewer model library.
bool trackSelected(const Document &, const OrbitCamera &, SceneToolState &);
bool updateTrackedCamera(const Document &, OrbitCamera &, SceneToolState &);
void stopTracking(SceneToolState &) noexcept;
bool applyRecordedCamera(const Document &, const std::string &name,
                         OrbitCamera &, SceneToolState &);
// Content only. The host owns Begin/End, calls Document::advance once and sets
// viewport_aspect for Fit actions. No network or capture state is modified.
void drawSceneTools(Document &, OrbitCamera &, SceneToolState &);
} // namespace pvd::viewer
