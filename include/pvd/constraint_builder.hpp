#pragma once
#include "pvd/debug_render.hpp"

namespace pvd::render {
struct ConstraintBuildOptions {
  // World-unit display scales. The wire carries poses/limits, not these viewer
  // scales. Zero disables the corresponding family; negative/nonfinite fails.
  float frame_scale = 1, limit_scale = 1;
  std::size_t max_constraints = 1024u * 1024u;
  std::size_t max_lines = 1024u * 1024u;
  std::size_t max_diagnostics = 256;
};
enum class ConstraintCode { invalid_payload, limit_exceeded };
struct ConstraintDiagnostic {
  ConstraintCode code = ConstraintCode::invalid_payload;
  DebugSource source;
  std::size_t constraint_index = 0;
  std::string message;
};
struct ConstraintLines {
  std::vector<DebugLine> lines;
  std::vector<ConstraintDiagnostic> diagnostics;
  std::size_t constraints_processed = 0, constraints_emitted = 0;
  bool complete = true;
};
// PhysX 3.4 CmVisualization's native line primitives, in SDK order. Retains
// world-space poses, active/inactive ARGB colors and exact source for picking.
// Arrowhead triangles are not converted into invented line edges. All output
// lines have derived=true. No history/range inference: pass this frame's typed
// constraints only, and disable extract_debug's separate joint-axis lines.
// Linear value is signed distance; angular limits/double cone are radians.
// LimitCone's y_swing/z_swing are wire tan(quarter-angle), NOT radians.
// Invalid constraints produce no partial geometry; valid later constraints may
// still render. A work/line budget ends at a complete constraint boundary.
// Invalid options throw invalid_argument. Payload failures are diagnostics.
PVD_RENDER_API ConstraintLines build_constraint_lines(
    const std::vector<DebugConstraint> &, ConstraintBuildOptions options = {});
} // namespace pvd::render
