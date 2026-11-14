#pragma once
#include "pvd/render.hpp"

namespace pvd::render {
enum class BodyOverlayKind { bounds, linear_velocity, angular_velocity };
enum class BodyOverlayPart { bounds_edge, vector_shaft, arrow_head };
struct BodyOverlaySource {
  // A real visible actor/shape attachment. Vector arrows choose one visible
  // attachment per (scene generation,actor generation); no fake PxShape.
  PickingKey key;
  std::size_t snapshot_event_count = 0;
};
struct BodyOverlayLine {
  BodyOverlaySource source;
  BodyOverlayKind kind = BodyOverlayKind::bounds;
  BodyOverlayPart part = BodyOverlayPart::bounds_edge;
  Vec3 position0, position1;
  std::uint32_t color = 0;
  bool derived = true;
};
enum class BodyOverlayCode {
  snapshot_mismatch,
  stale_source,
  missing_bounds,
  invalid_bounds,
  missing_property,
  invalid_property,
  limit_exceeded
};
struct BodyOverlayDiagnostic {
  BodyOverlayCode code = BodyOverlayCode::invalid_property;
  BodyOverlaySource source;
  std::string property, message;
};
struct BodyOverlayOptions {
  bool show_bounds = false, show_linear_velocity = false,
       show_angular_velocity = false;
  float velocity_scale = .2f;
  std::size_t max_lines = 262144, max_attachments = 262144,
              max_diagnostics = 256;
  // nullopt=all Frame attachments; present empty=none. Filter before querying
  // properties, so hidden objects do not leak arrows or diagnostics.
  std::optional<std::vector<PickingKey>> visible_attachments;
};
struct BodyOverlays {
  std::vector<BodyOverlayLine> bounds, linear_velocity, angular_velocity;
  std::vector<BodyOverlayDiagnostic> diagnostics;
  std::size_t bounds_count = 0, linear_vectors = 0, angular_vectors = 0,
              processed_attachments = 0;
  bool complete = true;
};
// Requested display layers only; no work is cached in or written to the source.
// Frame.event_count must match Snapshot.event_count. Bounds use the supplied
// tight geometry world_bounds (12 edges), never broadphase inflation.
// Captured velocities are world-space and anchored at Actor.GlobalPose.p;
// each nonzero vector emits one shaft plus two derived arrow-head lines.
// Static actors are skipped. Missing/invalid requested dynamic/link fields are
// diagnostic; another position/velocity is never substituted. All line families
// share max_lines, with complete AABBs/arrows committed atomically.
PVD_RENDER_API BodyOverlays build_body_overlays(
    const Frame &, const Snapshot &, const BodyOverlayOptions &options = {});
} // namespace pvd::render
