#pragma once
#include "model.hpp"
#include "pvd/memory.hpp"
#include <functional>
#include <iosfwd>

namespace pvd::viewer {
enum class AnalysisPage { profile, memory, errors };
// The host invokes the writer synchronously after choosing a destination.
using CsvExport = std::function<void(const char*,const std::function<void(std::ostream&)>&)>;
struct ProfileTimeSelection {
  std::uint64_t begin_ticks = 0, end_ticks = 0, duration_ticks = 0;
  std::optional<double> duration_nanoseconds;
};
struct ProfileTimelineView {
  float left = 0, top = 0, width = 0, height = 0;
  std::uint64_t begin_ticks = 0, end_ticks = 0;
};
// Last actual widget bounds, copied without exposing ImGui/session references.
// Zero dimensions mean that the control was not submitted by this draw.
struct ProfileControlRect {
  float x = 0, y = 0, width = 0, height = 0;
};
struct ProfilePanelControls {
  ProfileControlRect zone_combo, csv, measured_csv;
  ProfileControlRect palette_header, palette_by_event, palette_rgba,
      palette_restore;
};
class AnalysisPanels {
public:
  AnalysisPanels();
  ~AnalysisPanels();
  void reset();
  void draw(Document&,AnalysisPage,const CsvExport&);
  std::size_t row_count(AnalysisPage) const noexcept;
  std::size_t visible_row_count(AnalysisPage) const noexcept;
  // Last displayed native receipt interval. Copied value, not a Session/UI
  // reference; unavailable until the Memory page has a preceding native frame.
  std::optional<memory::WindowStats> memory_window_statistics() const;
  std::optional<ProfileTimeSelection> profile_time_selection() const;
  ProfileTimelineView profile_timeline_view() const noexcept;
  ProfilePanelControls profile_controls() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
