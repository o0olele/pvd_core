#pragma once
#include "analysis_panels.hpp"
#include "pvd/curves.hpp"

namespace pvd::viewer {
enum class CurvePage { curves, diff };
// Frame-axis coordinates are user-visible one-based frame numbers. The public
// series/seek/diff APIs retain zero-based source indices.
struct CurvePlotView {
  float left = 0, top = 0, width = 0, height = 0;
  double x_min = 0, x_max = 1, y_min = 0, y_max = 1;
  bool time_axis = false, valid = false;
};
// Content-only ImGui panels. Query work is explicit or throttled (at most once
// per second for live auto-refresh), capped at <=2000 rows per series by
// default.
class CurvePanels {
public:
  CurvePanels();
  ~CurvePanels();
  void reset();
  void draw(Document &, CurvePage, const CsvExport &);
  // Scripting/smoke path uses exactly the same sampling/export/seek model as
  // UI. Sources are bound to the Document's current scene and sample mode.
  std::size_t add_series(Document &, analysis::CurveRequest,
                         std::string label = {});
  std::size_t add_inspected(Document &, const std::string &property,
                            const std::string &component = {},
                            std::size_t item_index = 0);
  bool erase_series(std::size_t);
  void set_series_visible(std::size_t, bool);
  void set_auto_refresh(bool) noexcept;
  void refresh(Document &);
  std::size_t series_count() const noexcept;
  // Returned views last until the next sampling, erase, reset or draw that
  // refreshes data. Callers must not retain them across mutations.
  const analysis::Series *series(std::size_t) const noexcept;
  bool seek_sample(Document &, std::size_t series_index,
                   std::size_t sample_index);
  void set_time_axis(bool) noexcept;
  CurvePlotView plot_view() const noexcept;
  void set_diff_frames(std::size_t a, std::size_t b) noexcept;
  void refresh_diff(Document &);
  const analysis::StateDiff *diff_result() const noexcept;
  // Curves: sampled rows including gaps; visible rows: numeric points in the
  // plot range of enabled series. Diff: object rows / rendered table rows.
  std::size_t row_count(CurvePage page = CurvePage::curves) const noexcept;
  std::size_t
  visible_row_count(CurvePage page = CurvePage::curves) const noexcept;
  const std::string &error() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
