#include "export_job.hpp"
#ifdef PVD_VIEWER_HAS_ANALYSIS
#include "pvd/memory_bootstrap.hpp"
#include "pvd/profile_bootstrap.hpp"
#endif
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace pvd::viewer {
namespace {
using Clock = std::chrono::steady_clock;
struct Cancelled {};
} // namespace
const char *export_stage_name(ExportStage stage) noexcept {
  switch (stage) {
  case ExportStage::idle:
    return "Ready";
  case ExportStage::preparing:
    return "Preparing";
  case ExportStage::writing:
    return "Writing";
  case ExportStage::completed:
    return "Completed";
  case ExportStage::cancelled:
    return "Cancelled";
  case ExportStage::failed:
    return "Failed";
  }
  return "Unknown";
}
struct ExportJob::Impl {
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic<bool> working{false}, cancel_requested{false};
  ExportStatus state;
  Clock::time_point started{}, ended{};
  void check_cancel() const {
    if (cancel_requested.load(std::memory_order_relaxed))
      throw Cancelled{};
  }
  void stage(ExportStage value, const char *message) {
    std::lock_guard<std::mutex> lock(mutex);
    state.stage = value;
    state.message = message;
  }
  void warnings(std::vector<std::string> values) {
    std::lock_guard<std::mutex> lock(mutex);
    state.warnings = std::move(values);
  }
  void finish(const writing::WriteResult &result) {
    std::lock_guard<std::mutex> lock(mutex);
    state.result = result;
    state.has_result = true;
    state.stage = result.state == writing::WriteState::completed
                      ? ExportStage::completed
                      : ExportStage::cancelled;
    state.message.clear();
    ended = Clock::now();
  }
  void stop() {
    std::lock_guard<std::mutex> lock(mutex);
    state.stage = ExportStage::cancelled;
    state.message.clear();
    ended = Clock::now();
  }
  void fail(const char *message) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex);
      state.stage = ExportStage::failed;
      state.message.clear();
      ended = Clock::now();
      try {
        state.error = message;
      } catch (...) {
        state.error.clear();
      }
    } catch (...) {
    }
  }
  void run(const Session &session, ExportRequest request) noexcept {
    // working=false is published only after every borrowed-Session access and
    // exception cleanup has ended. The host can then append/replace safely.
    struct Complete {
      std::atomic<bool> &working;
      ~Complete() { working.store(false, std::memory_order_release); }
    } complete{working};
    try {
      check_cancel();
      if (request.destination.empty())
        throw std::invalid_argument(
            "Choose an output file before starting export.");
      auto options = request.write_options;
      const auto other_cancel = options.cancelled;
      options.cancelled = [this, other_cancel] {
        return cancel_requested.load(std::memory_order_relaxed) ||
               (other_cancel && other_cancel());
      };
      if (request.mode == ExportMode::whole) {
        stage(ExportStage::preparing,
              "Preparing the complete safe event prefix...");
        check_cancel();
        stage(ExportStage::writing,
              "Writing raw recording to a private temporary file...");
        finish(writing::save_session(request.destination, session, options,
                                     request.overwrite));
        return;
      }
      if (request.mode != ExportMode::native_frame_range)
        throw std::invalid_argument("Unknown export mode.");
      if (!request.scene_id)
        throw std::invalid_argument(
            "Choose a scene with native SDK frames before cropping.");
      stage(ExportStage::preparing,
            "Finding the selected scene's native frame range...");
      std::vector<std::size_t> frames;
      const auto &all_frames = session.frames();
      for (std::size_t i = 0; i < all_frames.size(); ++i)
        if (all_frames[i].scene_id == *request.scene_id)
          frames.push_back(i);
      if (request.first_native_frame > request.last_native_frame ||
          request.last_native_frame >= frames.size())
        throw std::out_of_range(
            "Native frame range is outside this scene's recorded frames.");
      const auto &first = all_frames[frames[request.first_native_frame]];
      const auto &last = all_frames[frames[request.last_native_frame]];
      if (!first.begin_event_count)
        throw writing::CropUnsupported(
            "The first native frame has no source begin marker.");
      const auto begin = first.begin_event_count - 1, end = last.event_count;
      check_cancel();
      writing::CropOptions crop_options;
      std::vector<std::string> notices;
      if (begin) {
        bool has_memory = false, has_profile = false;
        for (const auto &packet : session.channels()) {
          if (packet.event_index >= begin)
            break;
          has_memory = has_memory || packet.semantic == "memory event stream";
          has_profile =
              has_profile || packet.semantic == "profile event stream";
        }
#ifdef PVD_VIEWER_HAS_ANALYSIS
        if (has_memory) {
          stage(ExportStage::preparing,
                "Reconstructing memory dictionaries and live allocations...");
          memory::BootstrapOptions settings;
          if (request.allow_partial_memory)
            settings.unknown_frees = memory::UnknownFreePolicy::allow_partial;
          auto baseline = memory::make_bootstrap(session, begin, settings);
          check_cancel();
          crop_options.allow_partial_analysis = baseline.partial;
          for (auto &source : baseline.sources) {
            writing::ChannelBootstrap channel;
            channel.stream_id = source.source.stream_id;
            channel.instance_id = source.source.buffer_id;
            channel.instance_generation = source.buffer_generation;
            channel.property_name = source.property_name;
            channel.semantic = "memory event stream";
            channel.bytes = std::move(source.bytes);
            for (const auto &diagnostic : baseline.diagnostics)
              if (diagnostic.source == source.source) {
                channel.partial = true;
                channel.warnings.push_back(diagnostic.message);
              }
            crop_options.channel_bootstrap.push_back(std::move(channel));
          }
        }
        if (has_profile) {
          stage(ExportStage::preparing,
                "Reconstructing profiler scopes and counter state...");
          auto baseline = profile::make_bootstrap(session, begin);
          check_cancel();
          for (auto &source : baseline.sources) {
            writing::ChannelBootstrap channel;
            channel.stream_id = source.stream_id;
            channel.instance_id = source.zone.id;
            channel.instance_generation = source.buffer_generation;
            channel.property_name = source.property_name;
            channel.semantic = "profile event stream";
            channel.bytes = std::move(source.bytes);
            crop_options.channel_bootstrap.push_back(std::move(channel));
          }
          for (const auto &diagnostic : baseline.diagnostics)
            notices.push_back("Profile: " + diagnostic.message);
        }
#else
        if (has_memory || has_profile)
          throw writing::CropUnsupported(
              "This build has no analysis bootstrap support. Save the whole "
              "recording or use an analysis-enabled build for this range.");
#endif
      }
      stage(ExportStage::preparing,
            "Building and validating the cropped recording...");
      check_cancel();
      auto crop = writing::crop_events(session, begin, end, crop_options);
      check_cancel();
      notices.insert(notices.end(), crop.warnings.begin(), crop.warnings.end());
      warnings(std::move(notices));
      stage(ExportStage::writing,
            "Writing cropped raw recording to a private temporary file...");
      check_cancel();
      finish(writing::save_crop(request.destination, crop, options,
                                request.overwrite));
    } catch (const Cancelled &) {
      try {
        stop();
      } catch (...) {
        fail("Unable to finalize cancellation.");
      }
    } catch (const std::exception &e) {
      fail(e.what());
    } catch (...) {
      fail("Unknown export failure.");
    }
  }
};
ExportJob::ExportJob() : impl_(std::make_unique<Impl>()) {}
ExportJob::~ExportJob() {
  cancel();
  wait();
}
void ExportJob::start(const Session &session, ExportRequest request) {
  if (running())
    throw std::logic_error("An export is already running.");
  wait();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state = {};
    impl_->state.stage = ExportStage::preparing;
    impl_->state.destination = request.destination;
    impl_->state.message = "Starting background export...";
    impl_->started = Clock::now();
    impl_->ended = {};
  }
  impl_->cancel_requested.store(false, std::memory_order_relaxed);
  impl_->working.store(true, std::memory_order_release);
  try {
    impl_->worker = std::thread([state = impl_.get(), &session,
                                 request = std::move(request)]() mutable {
      state->run(session, std::move(request));
    });
  } catch (const std::exception &e) {
    impl_->fail(e.what());
    impl_->working.store(false, std::memory_order_release);
    throw;
  } catch (...) {
    impl_->fail("Unable to start export worker.");
    impl_->working.store(false, std::memory_order_release);
    throw;
  }
}
void ExportJob::cancel() noexcept {
  impl_->cancel_requested.store(true, std::memory_order_relaxed);
}
bool ExportJob::running() const noexcept {
  return impl_->working.load(std::memory_order_acquire);
}
ExportStatus ExportJob::status() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto result = impl_->state;
  result.cancel_requested =
      impl_->cancel_requested.load(std::memory_order_relaxed);
  if (result.stage != ExportStage::idle)
    result.elapsed_seconds =
        std::chrono::duration<double>(
            (result.terminal() ? impl_->ended : Clock::now()) - impl_->started)
            .count();
  return result;
}
void ExportJob::wait() {
  if (impl_->worker.joinable())
    impl_->worker.join();
}
} // namespace pvd::viewer
