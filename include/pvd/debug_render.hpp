#pragma once
#include "pvd/render_export.hpp"
#include "pvd/session.hpp"

namespace pvd::render {

struct DebugSource {
  std::uint64_t stream_id = 0, channel_instance_id = 0, instance_id = 0;
  std::size_t event_index = 0, payload_offset = 0;
};
struct DebugPoint {
  DebugSource source;
  Vec3 position;
  std::uint32_t color = 0;
};
struct DebugLine {
  DebugSource source;
  Vec3 position0, position1;
  std::uint32_t color0 = 0, color1 = 0;
  bool derived = false;
};
struct DebugTriangle {
  DebugSource source;
  Vec3 position0, position1, position2;
  std::uint32_t color0 = 0, color1 = 0, color2 = 0;
};
struct DebugText {
  DebugSource source;
  Vec3 position;
  float size = 0;
  std::uint32_t color = 0;
  std::string text;
};
struct DebugJointFrames {
  Transform parent, child;
};
struct DebugLinearLimit {
  Transform t0, t1;
  float value = 0;
  bool active = false;
};
struct DebugAngularLimit {
  Transform t0;
  float lower = 0, upper = 0;
  bool active = false;
};
struct DebugLimitCone {
  Transform transform;
  float y_swing = 0, z_swing = 0;
  bool active = false;
};
struct DebugDoubleCone {
  Transform transform;
  float angle = 0;
  bool active = false;
};
using DebugConstraintPayload =
    std::variant<DebugJointFrames, DebugLinearLimit, DebugAngularLimit,
                 DebugLimitCone, DebugDoubleCone>;
struct DebugConstraint {
  DebugSource source;
  DebugConstraintPayload payload;
};
struct DebugContact {
  std::uint64_t scene_id = 0, shape0 = 0, shape1 = 0;
  Vec3 position, normal;
  float separation = 0;
  std::optional<float> normal_force;
  std::uint32_t internal_face_index0 = 0, internal_face_index1 = 0;
};
enum class DebugCode {
  malformed,
  limit_exceeded,
  unsupported_event,
  unsupported_contacts,
  missing_scene,
  invalid_range,
  unattributed_scene,
  constraint_not_tessellated
};
struct DebugDiagnostic {
  DebugCode code = DebugCode::malformed;
  std::string message;
  std::size_t event_index = 0, payload_offset = 0;
  std::uint64_t channel_instance_id = 0;
};
struct DebugOptions {
  std::size_t max_primitives = 1024u * 1024u;
  std::size_t max_contacts = 1024u * 1024u;
  std::size_t max_packet_bytes = 64u * 1024u * 1024u;
  std::size_t max_text_bytes = 1024u * 1024u;
  std::size_t max_events_per_packet = 1024u * 1024u;
  // Positive values convert joint-frame axes to lines; zero retains typed data
  // only.
  float joint_axis_length = 0;
};
struct DebugRange {
  // Zero-based event interval [begin_event_count, end_event_count).
  std::size_t begin_event_count = 0, end_event_count = 0;
  std::optional<std::uint64_t> scene_id;
  // Explicit scene-owned renderer IDs can attribute otherwise unowned draw
  // calls. Without this evidence, a scene-filtered result omits unassociated
  // primitives.
  std::vector<std::uint64_t> scene_renderer_ids;
};
struct DebugResult {
  std::vector<DebugPoint> points;
  std::vector<DebugLine> lines;
  std::vector<DebugTriangle> triangles;
  std::vector<DebugText> texts;
  std::vector<DebugConstraint> constraints;
  std::vector<DebugContact> contacts;
  std::vector<DebugDiagnostic> diagnostics;
  bool complete = true, contacts_available = false;
};
// PxScene.Contacts is a 3.4 native PvdContact array: stride 52 (32-bit) or 64
// (64-bit), selected from the incoming PropertyValue, never sizeof(host type).
PVD_RENDER_API DebugResult extract_contacts(const Snapshot &, const Metadata &,
                                            std::uint64_t scene_id,
                                            DebugOptions options = {});
// The packet sequence must be ordered by event_index. Earlier packets seed only
// SetInstanceId state per (stream, renderer); their old primitives are not
// emitted. Unknown tags stop their packet with a diagnostic; they cannot be
// length-skipped.
PVD_RENDER_API DebugResult extract_debug(const Snapshot &, const Metadata &,
                                         const std::vector<ChannelPacket> &,
                                         DebugRange range,
                                         DebugOptions options = {});
PVD_RENDER_API DebugResult
extract_debug(const Session &, const FrameMarker &, DebugOptions options = {},
              std::vector<std::uint64_t> scene_renderer_ids = {});

} // namespace pvd::render
