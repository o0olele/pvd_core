#include "load_job.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace pvd::viewer {
namespace {
using Clock = std::chrono::steady_clock;
struct Cancelled {};
void path_text(const std::string &path) {
  if (path.empty() || path.size() > 1024u * 1024u ||
      path.find('\0') != std::string::npos)
    throw std::invalid_argument("Capture filename is empty, contains NUL, or "
                                "exceeds the path byte budget.");
  for (std::size_t i = 0; i < path.size();) {
    auto a = static_cast<unsigned char>(path[i++]);
    if (a < 0x80)
      continue;
    unsigned n = 0;
    std::uint32_t c = 0, minimum = 0;
    if (a >= 0xc2 && a <= 0xdf) {
      n = 1;
      c = a & 31u;
      minimum = 0x80;
    } else if (a >= 0xe0 && a <= 0xef) {
      n = 2;
      c = a & 15u;
      minimum = 0x800;
    } else if (a >= 0xf0 && a <= 0xf4) {
      n = 3;
      c = a & 7u;
      minimum = 0x10000;
    } else
      throw std::invalid_argument("Capture filename is not valid UTF-8.");
    if (n > path.size() - i)
      throw std::invalid_argument("Capture filename has truncated UTF-8.");
    while (n--) {
      auto b = static_cast<unsigned char>(path[i++]);
      if ((b & 0xc0) != 0x80)
        throw std::invalid_argument(
            "Capture filename has invalid UTF-8 continuation.");
      c = (c << 6) | (b & 63u);
    }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      throw std::invalid_argument(
          "Capture filename has an invalid UTF-8 scalar.");
  }
}
} // namespace
const char *load_stage_name(LoadStage stage) noexcept {
  switch (stage) {
  case LoadStage::idle:
    return "Ready to open";
  case LoadStage::reading:
    return "Reading";
  case LoadStage::decoding:
    return "Decoding";
  case LoadStage::preparing_scene:
    return "Preparing scene";
  case LoadStage::fingerprinting:
    return "Fingerprinting";
  case LoadStage::ready:
    return "Ready";
  case LoadStage::cancelled:
    return "Cancelled";
  case LoadStage::failed:
    return "Failed";
  }
  return "Unknown";
}
struct LoadJob::Impl {
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic<bool> working{false}, cancel_requested{false};
  LoadStatus state;
  std::optional<LoadedDocument> result;
  Clock::time_point started{}, ended{};
  void check(const LoadRequest &request) const {
    if (cancel_requested.load(std::memory_order_relaxed) ||
        (request.cancelled && request.cancelled()) ||
        cancel_requested.load(std::memory_order_relaxed))
      throw Cancelled{};
  }
  void stage(LoadStage stage, const char *message) {
    std::lock_guard<std::mutex> lock(mutex);
    state.stage = stage;
    state.message = message;
  }
  void progress(std::uint64_t bytes, std::size_t accepted,
                std::size_t decoded) {
    std::lock_guard<std::mutex> lock(mutex);
    state.bytes_read = bytes;
    state.event_count = accepted;
    state.decoded_event_count = decoded;
  }
  void stop() {
    std::lock_guard<std::mutex> lock(mutex);
    result.reset();
    state.has_result = false;
    state.stage = LoadStage::cancelled;
    state.message.clear();
    ended = Clock::now();
  }
  void fail(const char *text,
            std::optional<std::uint64_t> offset = {}) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex);
      result.reset();
      state.has_result = false;
      state.stage = LoadStage::failed;
      state.message.clear();
      state.error_byte_offset = offset;
      ended = Clock::now();
      try {
        state.error = text;
      } catch (...) {
        state.error.clear();
      }
    } catch (...) {
    }
  }
  void run(LoadRequest request) noexcept {
    struct Complete {
      std::atomic<bool> &flag;
      ~Complete() { flag.store(false, std::memory_order_release); }
    } complete{working};
    try {
      path_text(request.source_path);
      if (!request.target_slot)
        throw std::invalid_argument(
            "Capture load requires a nonzero target slot ID.");
      if (!request.max_file_bytes || !request.read_buffer_bytes ||
          request.read_buffer_bytes > 1024u * 1024u ||
          !request.events_per_step || request.events_per_step > 65536)
        throw std::invalid_argument(
            "Invalid file, read-buffer, or event-step limits.");
      stage(LoadStage::reading, "Inspecting capture file...");
      check(request);
      const auto path = std::filesystem::u8path(request.source_path);
      std::error_code error;
      const auto info = std::filesystem::status(path, error);
      if (error)
        throw std::runtime_error("Cannot inspect capture file: " +
                                 error.message());
      if (!std::filesystem::exists(info))
        throw std::runtime_error("Capture file was not found.");
      if (!std::filesystem::is_regular_file(info))
        throw std::runtime_error(
            "Capture input must be a regular file, not a directory or device.");
      const auto initial_size = std::filesystem::file_size(path);
      {
        std::lock_guard<std::mutex> lock(mutex);
        state.file_size = initial_size;
      }
      if (initial_size > request.max_file_bytes)
        throw std::length_error(
            "Capture file exceeds the configured maximum file size.");
      const auto initial_time = std::filesystem::last_write_time(path);
      check(request);
      std::ifstream input(path, std::ios::binary);
      if (!input)
        throw std::runtime_error("Unable to open capture file for reading.");
      StreamDecoder decoder(request.session_options.decode_limits);
      auto document = std::make_unique<Document>(request.model_options);
      bool initialized = false;
      std::vector<std::uint8_t> buffer(request.read_buffer_bytes);
      std::uint64_t read = 0;
      auto append = [&] {
        if (!initialized && decoder.header()) {
          check(request);
          document->begin_live(*decoder.header(), request.source_path,
                               request.session_options);
          document->set_follow_live(false);
          initialized = true;
        }
        auto events = decoder.take_events();
        decoder.take_group_boundaries();
        for (std::size_t i = 0; i < events.size();) {
          check(request);
          std::vector<Event> batch;
          const auto count =
              std::min(request.events_per_step, events.size() - i);
          batch.reserve(count);
          for (std::size_t j = 0; j < count; ++j)
            batch.push_back(std::move(events[i++]));
          try {
            document->append_live(std::move(batch));
          } catch (...) {
            progress(read, document->session()->event_count(),
                     decoder.event_count());
            throw;
          }
          progress(read, document->session()->event_count(),
                   decoder.event_count());
        }
        progress(read, initialized ? document->session()->event_count() : 0,
                 decoder.event_count());
      };
      while (true) {
        stage(LoadStage::reading, "Reading capture bytes...");
        check(request);
        input.read(reinterpret_cast<char *>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto n = static_cast<std::size_t>(input.gcount());
        if (n > std::numeric_limits<std::uint64_t>::max() - read)
          throw std::length_error("Capture byte counter exceeds uint64 range.");
        read += n;
        progress(read, initialized ? document->session()->event_count() : 0,
                 decoder.event_count());
        if (read > request.max_file_bytes)
          throw std::length_error("Capture grew beyond the configured maximum "
                                  "file size while reading.");
        if (n) {
          stage(LoadStage::decoding,
                "Decoding complete wire groups and indexing events...");
          check(request);
          try {
            decoder.feed(buffer.data(), n);
          } catch (...) {
            progress(read, initialized ? document->session()->event_count() : 0,
                     decoder.event_count());
            throw;
          }
          check(request);
          append();
        }
        if (input.bad() || (!input && !input.eof()))
          throw std::runtime_error("Capture read failed before end of file.");
        if (input.eof())
          break;
      }
      check(request);
      if (read != initial_size ||
          std::filesystem::file_size(path) != initial_size ||
          std::filesystem::last_write_time(path) != initial_time)
        throw std::runtime_error("Capture changed while loading. Retry after "
                                 "the recording has stopped.");
      stage(LoadStage::decoding, "Validating the final wire boundary...");
      decoder.finish();
      append();
      check(request);
      if (!initialized)
        throw std::runtime_error("Capture has no complete stream header.");
      stage(LoadStage::preparing_scene,
            "Preparing the initial scene; this bounded frame step may finish "
            "before cancellation...");
      check(request);
      document->finish_live();
      check(request);
      // Suppressed follow-live ingestion built only incremental indices.
      // Prepare one final native-frame/EOF snapshot, matching normal offline
      // loading.
      if (!document->scenes().empty())
        document->set_scene(document->scenes().front().key.id);
      document->set_sample_point(SamplePoint::native_frame);
      check(request);
      stage(LoadStage::fingerprinting,
            "Computing canonical capture identity...");
      auto identity = workspace_source_identity(
          *document->session(), [&](std::size_t done, std::size_t) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              state.fingerprint_events = done;
            }
            check(request);
          });
      check(request);
      {
        std::lock_guard<std::mutex> lock(mutex);
        result.emplace();
        result->target_slot = request.target_slot;
        result->source_path = std::move(request.source_path);
        result->document = std::move(document);
        result->identity = identity;
        state.stage = LoadStage::ready;
        state.has_result = true;
        state.message.clear();
        ended = Clock::now();
      }
    } catch (const Cancelled &) {
      try {
        stop();
      } catch (...) {
        fail("Unable to finalize load cancellation.");
      }
    } catch (const DecodeError &e) {
      fail(e.what(), e.offset());
    } catch (const SessionError &e) {
      fail(e.what(), e.byte_offset());
    } catch (const std::exception &e) {
      fail(e.what());
    } catch (...) {
      fail("Unknown capture load failure.");
    }
  }
};
LoadJob::LoadJob() : impl_(std::make_unique<Impl>()) {}
LoadJob::~LoadJob() {
  cancel();
  wait();
}
void LoadJob::start(LoadRequest request) {
  if (running())
    throw std::logic_error("A capture load is already running.");
  wait();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state = {};
    impl_->state.stage = LoadStage::reading;
    impl_->state.target_slot = request.target_slot;
    impl_->state.source_path = request.source_path;
    impl_->state.message = "Starting background load...";
    impl_->result.reset();
    impl_->started = Clock::now();
    impl_->ended = {};
  }
  impl_->cancel_requested.store(false, std::memory_order_relaxed);
  impl_->working.store(true, std::memory_order_release);
  try {
    impl_->worker = std::thread(
        [state = impl_.get(), request = std::move(request)]() mutable {
          state->run(std::move(request));
        });
  } catch (const std::exception &e) {
    impl_->fail(e.what());
    impl_->working.store(false, std::memory_order_release);
    throw;
  } catch (...) {
    impl_->fail("Unable to start capture load worker.");
    impl_->working.store(false, std::memory_order_release);
    throw;
  }
}
void LoadJob::cancel() noexcept {
  impl_->cancel_requested.store(true, std::memory_order_relaxed);
}
bool LoadJob::running() const noexcept {
  return impl_->working.load(std::memory_order_acquire);
}
LoadStatus LoadJob::status() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto status = impl_->state;
  status.cancel_requested =
      impl_->cancel_requested.load(std::memory_order_relaxed);
  if (status.stage != LoadStage::idle)
    status.elapsed_seconds =
        std::chrono::duration<double>(
            (status.terminal() ? impl_->ended : Clock::now()) - impl_->started)
            .count();
  return status;
}
std::optional<LoadedDocument> LoadJob::take_ready() {
  if (running())
    return {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->result)
    return {};
  auto result = std::move(impl_->result);
  impl_->result.reset();
  impl_->state.has_result = false;
  return result;
}
void LoadJob::wait() {
  if (impl_->worker.joinable())
    impl_->worker.join();
}
} // namespace pvd::viewer
