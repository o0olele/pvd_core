#pragma once
#include "model.hpp"
#include "renderer.hpp"

namespace pvd::viewer {
struct DeformablePanelState {
  double viewport_aspect = 1.0;
  char search[160]{};
  bool show_hidden = true, show_diagnostics = true;
  std::uint32_t element_index = 0;
  std::string error;
};
// Content only: caller owns ImGui Begin/End. Uses real owner/element keys;
// display controls and highlight never mutate the recording/session.
void drawDeformables(Document &, OrbitCamera &, RenderOptions &,
                     DeformablePanelState &);
} // namespace pvd::viewer
