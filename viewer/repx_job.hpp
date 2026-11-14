#pragma once
#include "pvd/repx.hpp"
#include <memory>

namespace pvd::viewer {
enum class RepxStage { idle, preparing, writing, completed, cancelled, failed };
const char *repx_stage_name(RepxStage) noexcept;
struct RepxRequest {
  std::string destination;
  // Exact displayed prefix, captured by the owner before starting the worker.
  std::size_t event_count = 0;
  repx::Options options;
  bool overwrite = false;
};
struct RepxStatus {
  RepxStage stage = RepxStage::idle;
  std::string destination, error;
  std::size_t event_count = 0, actors = 0, shapes = 0, materials = 0,
              resources = 0, xml_bytes = 0;
  std::vector<repx::Diagnostic> diagnostics;
  bool complete = false, cancel_requested = false;
  double elapsed_seconds = 0;
  bool terminal() const noexcept {
    return stage == RepxStage::completed || stage == RepxStage::cancelled ||
           stage == RepxStage::failed;
  }
};
// Session is borrowed: its owner freezes append/replacement/close until
// running()==false or wait(). Source options/selection are copied. The worker
// never accesses a Document or invokes a UI callback. Destruction cancels/joins.
class RepxJob {
public:
  RepxJob();
  ~RepxJob();
  RepxJob(const RepxJob &) = delete;
  RepxJob &operator=(const RepxJob &) = delete;
  void start(const Session &, RepxRequest);
  void cancel() noexcept;
  bool running() const noexcept;
  RepxStatus status() const;
  void wait();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
