#pragma once
#include "model.hpp"
#include "pvd/memory.hpp"
#include <functional>
#include <iosfwd>

namespace pvd::viewer {
enum class AnalysisPage { profile, memory, errors };
// The host invokes the writer synchronously after choosing a destination.
using CsvExport = std::function<void(const char*,const std::function<void(std::ostream&)>&)>;
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
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
