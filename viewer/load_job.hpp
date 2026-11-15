#pragma once
#include "workspace.hpp"
#include <functional>

namespace pvd::viewer {
enum class LoadStage {
  idle,
  reading,
  decoding,
  preparing_scene,
  fingerprinting,
  ready,
  cancelled,
  failed
};
const char *load_stage_name(LoadStage) noexcept;
struct LoadRequest {
  SlotId target_slot = 0;
  std::string source_path;
  std::uint64_t max_file_bytes = 4ull * 1024ull * 1024ull * 1024ull;
  std::size_t read_buffer_bytes = 64u * 1024u;
  std::size_t events_per_step = 256;
  SessionOptions session_options = [] {
    SessionOptions value;
    value.max_checkpoints = 0; // SnapshotView supplies indexed random access.
    return value;
  }();
  ModelOptions model_options;
  // Optional external cancellation, called on the worker at checkpoints. Any
  // captures must outlive the job; it must not call ImGui or block the UI.
  std::function<bool()> cancelled;
};
struct LoadStatus {
  LoadStage stage = LoadStage::idle;
  SlotId target_slot = 0;
  std::string source_path, message, error;
  std::optional<std::uint64_t> file_size;
  std::uint64_t bytes_read = 0;
  std::size_t event_count = 0, decoded_event_count = 0, fingerprint_events = 0;
  std::optional<std::uint64_t> error_byte_offset;
  bool cancel_requested = false, has_result = false;
  double elapsed_seconds = 0;
  bool terminal() const noexcept {
    return stage == LoadStage::ready || stage == LoadStage::cancelled ||
           stage == LoadStage::failed;
  }
};
struct LoadedDocument {
  SlotId target_slot = 0;
  std::string source_path;
  std::unique_ptr<Document> document;
  SourceIdentity identity;
};
// Owns only a fresh staging Document. Existing Workspace slots are never read
// or modified. Host polls status, takes a ready result without waiting, then
// replaces its stable target ID. Cancels/failures discard the entire candidate.
// Decoding checks between read chunks/append batches; one complete wire group
// (up to decode_limits.max_group_bytes) and final frame preparation remain
// bounded cooperative steps. Status reports measured work, no overall percent.
class LoadJob {
public:
  LoadJob();
  ~LoadJob();
  LoadJob(const LoadJob &) = delete;
  LoadJob &operator=(const LoadJob &) = delete;
  void start(LoadRequest);
  void cancel() noexcept;
  bool running() const noexcept;
  LoadStatus status() const;
  // Returns empty while running or after the single ready result was consumed.
  std::optional<LoadedDocument> take_ready();
  // Shutdown/tests only; the interactive frame loop never needs to wait.
  void wait();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
