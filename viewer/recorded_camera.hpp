#pragma once
#include "model.hpp"

namespace pvd::viewer {
struct SceneToolState;
enum class RecordedCameraStatus {
  inactive,
  following,
  missing,
  invalid,
  source_changed,
  released
};
struct RecordedCameraFollowState {
  bool enabled = false;
  const Document *document = nullptr;
  const Session *source = nullptr;
  std::string name, diagnostic;
  RecordedCameraStatus status = RecordedCameraStatus::inactive;
  std::optional<std::size_t> event_count;
  std::optional<CameraState> sample;
  std::optional<OrbitCamera> applied;
  void reset() noexcept {
    enabled = false;
    document = nullptr;
    source = nullptr;
    name.clear();
    diagnostic.clear();
    status = RecordedCameraStatus::inactive;
    event_count.reset();
    sample.reset();
    applied.reset();
  }
};
// Binding succeeds even if this prefix has no valid sample yet; status then
// reports missing/invalid, keeps the last view, and resumes on a later sample.
// The camera name is global in the SDK wire event, never inferred from scene.
bool followRecordedCamera(const Document &, const std::string &name,
                          OrbitCamera &, SceneToolState &);
// Called by updateTrackedCamera, after advance/seek and before rendering.
// Controls eye/target/up only; current FOV, clip range and handedness persist.
bool updateRecordedCameraFollow(const Document &, OrbitCamera &,
                                SceneToolState &);
void releaseRecordedCamera(SceneToolState &, const std::string &reason = {});
// Host calls before manual orbit/pan/zoom/WASD/look-at/up/fit/bookmark changes.
// Releases Clip camera only; object tracking retains its established offsets.
void notifyManualCameraInput(SceneToolState &);
} // namespace pvd::viewer
