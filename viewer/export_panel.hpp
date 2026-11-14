#pragma once
#include "export_job.hpp"
#include "model.hpp"
#include <functional>

namespace pvd::viewer {
struct ExportDestination {
  std::string path;
  // True only after the host explicitly obtained overwrite authorization.
  bool overwrite = false;
};
using ChooseExportPath = std::function<std::optional<ExportDestination>(
    const char *suggested_filename)>;
struct ExportPanelState {
  ExportMode mode = ExportMode::whole;
  int first_native_frame = 1, last_native_frame = 1;
  bool allow_partial_memory = false;
  std::string error;
  const Session *source = nullptr;
  std::optional<std::uint64_t> scene_id;
};
struct ExportPanelResult {
  bool started = false, close_requested = false;
};
// Content-only: the host owns its window/popup and choose-path dialog. The host
// freezes Document append/replacement during a job; transport reception and raw
// recording may continue independently while its displayed prefix is frozen.
ExportPanelResult draw_export_panel(Document &, ExportJob &, ExportPanelState &,
                                    const ChooseExportPath &);
// Compact=false includes destination/results/warnings. Safe for a task bar or
// popup and never waits for the worker. Cancel remains a request until
// terminal.
void draw_export_progress(ExportJob &, bool compact = false);
} // namespace pvd::viewer
