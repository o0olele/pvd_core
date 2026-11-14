#include "repx_job.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace pvd::viewer {
namespace { using Clock = std::chrono::steady_clock; }
const char *repx_stage_name(RepxStage s) noexcept {
  switch (s) {
  case RepxStage::idle: return "Ready";
  case RepxStage::preparing: return "Preparing RepX";
  case RepxStage::writing: return "Writing RepX";
  case RepxStage::completed: return "Saved RepX";
  case RepxStage::cancelled: return "Cancelled";
  case RepxStage::failed: return "Failed";
  }
  return "Unknown";
}
struct RepxJob::Impl {
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic<bool> working{false}, cancellation{false};
  RepxStatus state;
  Clock::time_point started{}, ended{};
  void terminal(RepxStage stage, const char *error = nullptr) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex);
      state.stage = stage;
      ended = Clock::now();
      if (error) state.error = error;
    } catch (...) {}
  }
  void run(const Session &source, RepxRequest request) noexcept {
    struct Done {
      std::atomic<bool> &working;
      ~Done() { working.store(false, std::memory_order_release); }
    } done{working};
    try {
      if (request.destination.empty())
        throw std::invalid_argument("Choose a RepX destination.");
      if (request.event_count > source.event_count())
        throw std::out_of_range("RepX prefix exceeds the source recording.");
      auto options = std::move(request.options);
      auto other_cancel = std::move(options.cancelled);
      options.cancelled = [this, other_cancel] {
        return cancellation.load(std::memory_order_relaxed) ||
               (other_cancel && other_cancel());
      };
      if (options.cancelled()) throw repx::ExportCancelled{};
      auto result = repx::export_session(source, request.event_count, options);
      {
        std::lock_guard<std::mutex> lock(mutex);
        state.stage = RepxStage::writing;
        state.actors = result.actor_count;
        state.shapes = result.shape_count;
        state.joints = result.joint_count;
        state.articulations = result.articulation_count;
        state.articulation_links = result.articulation_link_count;
        state.articulation_joints = result.articulation_joint_count;
        state.aggregates = result.aggregate_count;
        state.materials = result.material_count;
        state.resources = result.resource_count;
        state.xml_bytes = result.xml.size();
        state.diagnostics = result.diagnostics;
        state.complete = result.complete;
      }
      repx::save_file(request.destination, result, request.overwrite,
                      options.cancelled);
      terminal(RepxStage::completed);
    } catch (const repx::ExportCancelled &) {
      terminal(RepxStage::cancelled);
    } catch (const std::exception &e) {
      terminal(RepxStage::failed, e.what());
    } catch (...) {
      terminal(RepxStage::failed, "Unknown RepX export failure.");
    }
  }
};
RepxJob::RepxJob() : impl_(std::make_unique<Impl>()) {}
RepxJob::~RepxJob() { cancel(); wait(); }
void RepxJob::start(const Session &source, RepxRequest request) {
  if (running()) throw std::logic_error("A RepX export is already running.");
  wait();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state = {};
    impl_->state.stage = RepxStage::preparing;
    impl_->state.destination = request.destination;
    impl_->state.event_count = request.event_count;
    impl_->started = Clock::now();
    impl_->ended = {};
  }
  impl_->cancellation.store(false, std::memory_order_relaxed);
  impl_->working.store(true, std::memory_order_release);
  try {
    impl_->worker = std::thread([state = impl_.get(), &source,
                                 request = std::move(request)]() mutable {
      state->run(source, std::move(request));
    });
  } catch (...) {
    impl_->terminal(RepxStage::failed, "Unable to start RepX worker.");
    impl_->working.store(false, std::memory_order_release);
    throw;
  }
}
void RepxJob::cancel() noexcept {
  impl_->cancellation.store(true, std::memory_order_relaxed);
}
bool RepxJob::running() const noexcept {
  return impl_->working.load(std::memory_order_acquire);
}
RepxStatus RepxJob::status() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto state = impl_->state;
  state.cancel_requested = impl_->cancellation.load(std::memory_order_relaxed);
  if (state.stage != RepxStage::idle)
    state.elapsed_seconds = std::chrono::duration<double>(
        (state.terminal() ? impl_->ended : Clock::now()) - impl_->started).count();
  return state;
}
void RepxJob::wait() { if (impl_->worker.joinable()) impl_->worker.join(); }
} // namespace pvd::viewer
