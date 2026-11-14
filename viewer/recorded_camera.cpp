#include "recorded_camera.hpp"
#include "scene_tools.hpp"

namespace pvd::viewer {
namespace {
bool same(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool same(const CameraState &a, const CameraState &b) {
  return same(a.position, b.position) && same(a.target, b.target) &&
         same(a.up, b.up);
}
bool same_pose(const OrbitCamera &a, const OrbitCamera &b) {
  return same(a.target, b.target) && same(a.reference_up, b.reference_up) &&
         a.yaw == b.yaw && a.pitch == b.pitch && a.distance == b.distance;
}
void note(SceneToolState &s, RecordedCameraStatus status, std::string text) {
  s.recorded_follow.status = status;
  s.recorded_follow.diagnostic = std::move(text);
  s.message = s.recorded_follow.diagnostic;
}
} // namespace
void releaseRecordedCamera(SceneToolState &state, const std::string &reason) {
  if (!state.recorded_follow.enabled)
    return;
  const auto name = state.recorded_follow.name;
  state.recorded_follow.reset();
  note(state, RecordedCameraStatus::released,
       "Recorded camera released: " + name +
           (reason.empty() ? "" : " (" + reason + ")"));
}
void notifyManualCameraInput(SceneToolState &state) {
  releaseRecordedCamera(state, "manual camera control");
}
bool followRecordedCamera(const Document &doc, const std::string &name,
                          OrbitCamera &camera, SceneToolState &state) {
  if (!doc.loaded() || !doc.session()) {
    state.message = "Open a capture before following a recorded camera";
    return false;
  }
  if (name.empty()) {
    state.message = "Choose a recorded camera name";
    return false;
  }
  SceneToolState next = state;
  stopTracking(next);
  next.document_source = doc.session();
  next.recorded_camera = name;
  next.recorded_follow.enabled = true;
  next.recorded_follow.document = &doc;
  next.recorded_follow.source = doc.session();
  next.recorded_follow.name = name;
  next.error.clear();
  state = std::move(next);
  updateRecordedCameraFollow(doc, camera, state);
  return true;
}
bool updateRecordedCameraFollow(const Document &doc, OrbitCamera &camera,
                                SceneToolState &state) {
  auto &follow = state.recorded_follow;
  if (!follow.enabled)
    return false;
  if (!doc.loaded() || follow.document != &doc ||
      follow.source != doc.session()) {
    follow.reset();
    note(state, RecordedCameraStatus::source_changed,
         "Recorded camera follow stopped: capture/source changed");
    return false;
  }
  if (follow.applied && !same_pose(camera, *follow.applied)) {
    notifyManualCameraInput(state);
    return false;
  }
  const auto frame =
      doc.frame(); // Own the immutable snapshot while inspecting it.
  if (!frame)
    return false;
  follow.event_count = frame->snapshot.event_count;
  const auto found = frame->snapshot.cameras.find(follow.name);
  if (found == frame->snapshot.cameras.end()) {
    follow.sample.reset();
    note(state, RecordedCameraStatus::missing,
         "Waiting: recorded camera '" + follow.name +
             "' is absent at this prefix; keeping the last view");
    return false;
  }
  if (follow.sample && same(*follow.sample, found->second)) {
    note(state, RecordedCameraStatus::following,
         "Following recorded camera: " + follow.name);
    return false;
  }
  try {
    auto next = camera;
    next.set_look_at(found->second);
    // SetCamera contains no projection settings. Retain the user's settings.
    next.vertical_fov = camera.vertical_fov;
    next.near_plane = camera.near_plane;
    next.far_plane = camera.far_plane;
    camera = next;
    follow.applied = next;
    follow.sample = found->second;
    note(state, RecordedCameraStatus::following,
         "Following recorded camera: " + follow.name);
    state.error.clear();
    return true;
  } catch (const std::exception &e) {
    follow.sample.reset();
    note(state, RecordedCameraStatus::invalid,
         "Invalid recorded camera '" + follow.name + "': " + e.what() +
             "; keeping the last view");
    return false;
  }
}
} // namespace pvd::viewer
