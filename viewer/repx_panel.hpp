#pragma once
#include "export_panel.hpp"
#include "repx_job.hpp"
namespace pvd::viewer {
enum class RepxScope { current_scene, all_scenes, visible_shapes, selection };
struct RepxPanelState {
  RepxScope scope = RepxScope::current_scene;
  bool allow_partial = false;
  const Session *source = nullptr;
  std::string error;
};
// Pure selection adapter: validates/copies current frame prefix and exact
// generation-aware attachment keys. Does not start a worker or mutate the view.
RepxRequest make_repx_request(const Document &, RepxScope, bool allow_partial,
                             ExportDestination);
// Host freezes this source slot during a job, like the raw ExportJob.
ExportPanelResult draw_repx_panel(Document &, RepxJob &, RepxPanelState &,
                                  const ChooseExportPath &);
void draw_repx_progress(RepxJob &, bool compact = false);
} // namespace pvd::viewer
