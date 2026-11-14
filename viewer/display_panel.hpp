#pragma once
#include "model.hpp"
#include "renderer.hpp"

namespace pvd::viewer {
struct DisplayPanelState {
  std::string error;
  double viewport_aspect = 1.0;
};
// Content only. Pass the last rendered stats; the panel explicitly
// distinguishes a pending AA request from actual 1x/4x use and
// unsupported-device fallback.
void drawDisplaySettings(Document &, OrbitCamera &, RenderOptions &,
                         const RenderStats &, DisplayPanelState &);
} // namespace pvd::viewer
