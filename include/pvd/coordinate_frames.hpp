#pragma once
#include "pvd/render.hpp"

namespace pvd::render {
enum class CoordinateFrameKind { actor, shape, mass };
struct CoordinateFrameSource {
  PickingKey key;
  std::size_t snapshot_event_count = 0;
  CoordinateFrameKind kind = CoordinateFrameKind::actor;
};
struct CoordinateFrameLine {
  CoordinateFrameSource source;
  unsigned axis = 0; // native local X/Y/Z, not a display-axis permutation
  Vec3 position0, position1;
  std::uint32_t color = 0;
};
enum class CoordinateFrameCode {
  snapshot_mismatch,
  stale_source,
  missing_property,
  invalid_property,
  limit_exceeded
};
struct CoordinateFrameDiagnostic {
  CoordinateFrameCode code = CoordinateFrameCode::invalid_property;
  CoordinateFrameSource source;
  std::string property, message;
};
struct CoordinateFrameOptions {
  bool actor = false, shape = false, mass = false;
  float axis_length = .5f;
  std::size_t max_lines = 262144, max_attachments = 262144,
              max_diagnostics = 256;
  // nullopt=all supplied Frame attachments, present empty=none. Apply before
  // source-property inspection so hidden/filtered objects emit no diagnostics.
  std::optional<std::vector<PickingKey>> attachments;
};
struct CoordinateFrames {
  std::vector<CoordinateFrameLine> actor, shape, mass;
  std::vector<CoordinateFrameDiagnostic> diagnostics;
  std::size_t actor_count = 0, shape_count = 0, mass_count = 0,
              processed_attachments = 0;
  bool complete = true;
};
// Native Actor.GlobalPose uses the coordinates shadow after OriginShift.
// Shape.world_pose is already native; mass=actor pose * local CMassLocalPose.
// Actor/mass frames are deduplicated per actual (scene,actor) generation.
// Static bodies have no mass frame; missing dynamic/link COM is diagnostic.
// All frame families share max_lines; a complete three-axis triad is atomic.
PVD_RENDER_API CoordinateFrames
build_coordinate_frames(const Frame &, const Snapshot &,
                        const CoordinateFrameOptions &options = {});
PVD_RENDER_API CoordinateFrames
build_coordinate_frames(const Frame &, const SnapshotView &,
                        const CoordinateFrameOptions &options = {});
} // namespace pvd::render
