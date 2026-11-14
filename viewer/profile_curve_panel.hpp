#pragma once
#include "analysis_panels.hpp"
#include "pvd/profile_curves.hpp"

namespace pvd::viewer {
struct ProfileCurvePlotView {
  float left = 0, top = 0, width = 0, height = 0;
  double x_min = 1, x_max = 2, y_min = 0, y_max = 1;
  bool time_axis = false, valid = false;
  std::string unit; // count / ticks / ns; only this unit group is overlaid.
};
// Content only; the host owns Begin/End and serializes this with Session
// append. Always uses native scene windows, even while Document shows
// after-fetch data.
class ProfileCurvePanel {
public:
  ProfileCurvePanel();
  ~ProfileCurvePanel();
  void reset();
  void draw(Document &, const CsvExport &);
  // Bind scene to Document and force native sampling; other filters/ranges
  // stay.
  std::size_t add_series(Document &, analysis::ProfileCurveRequest,
                         std::string label = {});
  void set_request(Document &, std::size_t, analysis::ProfileCurveRequest);
  bool erase_series(std::size_t);
  void select_series(std::size_t);
  void set_series_visible(std::size_t, bool);
  void set_auto_refresh(bool) noexcept;
  void set_time_axis(bool) noexcept;
  void refresh(Document &);
  std::size_t series_count() const noexcept;
  // Views remain valid until the next mutation, source switch or auto-refresh.
  const analysis::ProfileCurveSeries *series(std::size_t) const noexcept;
  const analysis::ProfileCurveRequest *request(std::size_t) const noexcept;
  const profile::Index *profile_index() const noexcept;
  std::size_t indexed_event_count() const noexcept;
  // All sampled rows, including gaps; visible rows are numeric points in the
  // enabled series of the active unit group and plot range.
  std::size_t row_count() const noexcept;
  std::size_t visible_row_count() const noexcept;
  ProfileCurvePlotView plot_view() const;
  bool seek_sample(Document &, std::size_t series_index,
                   std::size_t sample_index);
  const std::string &error() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
