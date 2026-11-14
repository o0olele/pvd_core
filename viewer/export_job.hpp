#pragma once
#include "pvd/writer.hpp"
#include <memory>

namespace pvd::viewer {
enum class ExportMode { whole, native_frame_range };
enum class ExportStage {
  idle,
  preparing,
  writing,
  completed,
  cancelled,
  failed
};
const char *export_stage_name(ExportStage) noexcept;
struct ExportRequest {
  std::string destination;
  ExportMode mode = ExportMode::whole;
  std::optional<std::uint64_t> scene_id;
  // Inclusive, zero-based indices within this scene's native SDK frames.
  std::size_t first_native_frame = 0, last_native_frame = 0;
  bool overwrite = false;
  bool allow_partial_memory = false;
  // Copied into the worker. Any callback captures must outlive the job.
  writing::WriteOptions write_options;
};
struct ExportStatus {
  ExportStage stage = ExportStage::idle;
  std::string destination, message, error;
  std::vector<std::string> warnings;
  writing::WriteResult result;
  bool has_result = false, cancel_requested = false;
  double elapsed_seconds = 0;
  bool terminal() const noexcept {
    return stage == ExportStage::completed || stage == ExportStage::cancelled ||
           stage == ExportStage::failed;
  }
};
// UI-independent worker. Session is borrowed: the host must keep it alive and
// prohibit append/replacement from start() until running()==false or wait().
// Immutable frame/selection queries may continue. Commands run on one owner
// thread; status() and cancel() are safe while the worker runs. No UI callback
// executes on the worker. Destruction requests cancellation and joins.
class ExportJob {
public:
  ExportJob();
  ~ExportJob();
  ExportJob(const ExportJob &) = delete;
  ExportJob &operator=(const ExportJob &) = delete;
  void start(const Session &, ExportRequest);
  void cancel() noexcept;
  bool running() const noexcept;
  ExportStatus status() const;
  // For shutdown/tests only; do not block the interactive frame loop.
  void wait();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
