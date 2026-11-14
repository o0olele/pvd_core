#pragma once
#include "model.hpp"
#include "pvd/capture.hpp"

namespace pvd::viewer {
struct LivePollLimits {
  std::size_t max_events = 16384, max_payload_bytes = 8u * 1024u * 1024u;
};
// UI-independent coordinator. Socket IO, decoding and recording run on the
// transport worker. The host calls poll(Document&) about every 100ms; it copies
// only a bounded new event delta and incrementally advances the existing model.
// Document/command/poll access is serialized by the host, never concurrently.
class LiveSession {
public:
  explicit LiveSession(LivePollLimits limits = {});
  ~LiveSession();
  LiveSession(const LiveSession &) = delete;
  LiveSession &operator=(const LiveSession &) = delete;
  void start(CaptureOptions);
  void stop() noexcept; // Cancel only; no socket/network join on the UI thread.
  bool poll(Document &);      // True iff the displayed model frame changed.
  void set_follow_live(bool); // Applies once on the next poll; manual Document
                              // seek pauses follow.
  bool follow_live() const noexcept;
  bool active() const noexcept; // Includes importing a terminal transport's
                                // remaining safe delta.
  bool caught_up() const noexcept;
  std::size_t accepted_event_count() const noexcept;
  const CaptureStatus &status() const noexcept;
  const std::string &error() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
