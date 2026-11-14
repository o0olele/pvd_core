#include "live_session.hpp"
#include <algorithm>
#include <stdexcept>

namespace pvd::viewer {
struct LiveSession::Impl {
  LivePollLimits limits;
  CaptureOptions options;
  std::unique_ptr<TcpCapture> capture;
  CaptureStatus status;
  std::size_t cursor = 0;
  bool initialized = false, document_finished = false, import_failed = false;
  bool follow = true, follow_pending = false;
  std::string error;
  explicit Impl(LivePollLimits l) : limits(l) {
    if (!l.max_events || !l.max_payload_bytes)
      throw std::invalid_argument("live poll limits must be nonzero");
  }
  bool caught_up() const noexcept {
    return cursor == status.recoverable_event_count;
  }
  bool active() const noexcept {
    return capture && (!status.terminal() || (!import_failed && !caught_up()));
  }
  void publish_error(const std::string &text) {
    error = text;
    if (std::find(status.errors.begin(), status.errors.end(), error) ==
        status.errors.end())
      status.errors.push_back(error);
  }
};
LiveSession::LiveSession(LivePollLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {}
LiveSession::~LiveSession() = default;
void LiveSession::start(CaptureOptions options) {
  if (impl_->active())
    throw std::logic_error("live capture is active; stop and import its safe "
                           "tail before restarting");
  auto next = std::make_unique<Impl>(impl_->limits);
  next->options = std::move(options);
  next->capture = std::make_unique<TcpCapture>(next->options);
  // Binding and exclusive file creation are local setup only. Network receive,
  // decode and recording have their own worker once start returns.
  try {
    next->capture->start();
    next->status = next->capture->status();
  } catch (const std::exception &e) {
    next->status = next->capture->status();
    next->publish_error(e.what());
    impl_.swap(next);
    throw;
  }
  impl_.swap(next);
}
void LiveSession::stop() noexcept {
  if (impl_->capture)
    impl_->capture->cancel();
}
bool LiveSession::poll(Document &document) {
  if (!impl_->capture)
    return false;
  auto before = document.frame();
  if (impl_->import_failed) {
    impl_->status = impl_->capture->status();
    if (!impl_->error.empty() &&
        std::find(impl_->status.errors.begin(), impl_->status.errors.end(),
                  impl_->error) == impl_->status.errors.end())
      impl_->status.errors.push_back(impl_->error);
    return false;
  }
  try {
    auto delta =
        impl_->capture->poll_from(impl_->cursor, impl_->limits.max_events,
                                  impl_->limits.max_payload_bytes);
    impl_->status = std::move(delta.status);
    if (!impl_->import_failed) {
      if (delta.header && !impl_->initialized) {
        auto label = impl_->options.recording_path.empty()
                         ? "TCP " + impl_->status.address + ":" +
                               std::to_string(impl_->status.port)
                         : impl_->options.recording_path;
        document.begin_live(*delta.header, std::move(label),
                            impl_->options.session_options);
        impl_->initialized = true;
        document.set_follow_live(impl_->follow);
      }
      if (impl_->initialized && !impl_->document_finished) {
        if (document.live_event_count() != impl_->cursor)
          throw std::logic_error(
              "live Document was replaced while its capture was active");
        if (impl_->follow_pending) {
          document.set_follow_live(impl_->follow);
          impl_->follow_pending = false;
        }
        if (!delta.events.empty())
          document.append_live(std::move(delta.events));
        impl_->cursor = delta.end_event_count;
        impl_->follow = document.follow_live();
        if (impl_->status.terminal() && impl_->caught_up()) {
          document.finish_live();
          impl_->document_finished = true;
          impl_->follow = false;
        }
      }
    }
    if (!impl_->error.empty() &&
        std::find(impl_->status.errors.begin(), impl_->status.errors.end(),
                  impl_->error) == impl_->status.errors.end())
      impl_->status.errors.push_back(impl_->error);
  } catch (const std::exception &e) {
    impl_->import_failed = true;
    impl_->capture->cancel();
    try {
      impl_->status = impl_->capture->status();
    } catch (...) {
    }
    impl_->publish_error(e.what());
  }
  return document.frame() != before;
}
void LiveSession::set_follow_live(bool follow) {
  impl_->follow = follow;
  impl_->follow_pending = true;
}
bool LiveSession::follow_live() const noexcept { return impl_->follow; }
bool LiveSession::active() const noexcept { return impl_->active(); }
bool LiveSession::caught_up() const noexcept { return impl_->caught_up(); }
std::size_t LiveSession::accepted_event_count() const noexcept {
  return impl_->cursor;
}
const CaptureStatus &LiveSession::status() const noexcept {
  return impl_->status;
}
const std::string &LiveSession::error() const noexcept { return impl_->error; }
} // namespace pvd::viewer
