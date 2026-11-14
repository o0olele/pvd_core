#pragma once
#include "pvd/session.hpp"
#include <functional>
#include <iosfwd>
#include <limits>

namespace pvd::writing {
struct WriteOptions {
  std::size_t target_group_bytes = 1024u * 1024u;
  std::size_t max_group_bytes = 64u * 1024u * 1024u;
  std::size_t max_events_per_group = 1024u * 1024u;
  std::uint64_t max_output_bytes = std::numeric_limits<std::uint64_t>::max();
  // Called synchronously between events/groups. Exceptions propagate.
  std::function<bool()> cancelled;
};
enum class WriteState { completed, cancelled };
struct WriteResult {
  WriteState state = WriteState::completed;
  std::size_t event_count = 0, group_count = 0;
  std::uint64_t bytes_written = 0;
  std::size_t requested_event_count = 0, discarded_tail_events = 0;
};
struct SafePrefix {
  std::size_t requested_event_count = 0, event_count = 0;
  std::size_t discarded_tail_events = 0;
};
// No event reordering. Adjacent events share a group only when stream ID and
// timestamp match. Opaque property/message/channel bytes remain byte-exact;
// the header's byte order must describe their existing encoding.
PVD_CORE_API Bytes encode_header(const StreamHeader &);
PVD_CORE_API Bytes encode_event(const Event &, ByteOrder,
                                std::size_t max_bytes = 64u * 1024u * 1024u);
PVD_CORE_API WriteResult write_raw(std::ostream &, const StreamHeader &,
                                   const std::vector<Event> &,
                                   const WriteOptions & = {});
// Accepted Session source events suffice, even if state recovery failed.
// Pending chunk/message-group tails are excluded. Open sections are allowed.
PVD_CORE_API SafePrefix
safe_prefix(const Session &, std::size_t requested_event_count =
                                 std::numeric_limits<std::size_t>::max());
PVD_CORE_API WriteResult
write_session(std::ostream &, const Session &, const WriteOptions & = {},
              std::size_t requested_event_count =
                  std::numeric_limits<std::size_t>::max());
// UTF-8 path. A private exclusive-created sibling is published only after a
// successful close. Cancellation/failure leaves the destination untouched.
// Existing destinations are rejected unless overwrite is explicitly true.
PVD_CORE_API WriteResult
save_session(const std::string &utf8_path, const Session &,
             const WriteOptions & = {}, bool overwrite = false,
             std::size_t requested_event_count =
                 std::numeric_limits<std::size_t>::max());

class CropUnsupported : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
struct CroppedStream {
  StreamHeader header;
  std::vector<Event> events;
  std::size_t bootstrap_event_count = 0;
  std::size_t source_begin_event_count = 0, source_end_event_count = 0;
  std::vector<std::string> warnings;
};
// Optional analysis-layer reconstruction. The provider must include the full
// dictionary/open-state prefix required by later packets, not the last packet.
// The source stream and buffer generation are checked against the snapshot.
struct ChannelBootstrap {
  std::uint64_t stream_id = 0, instance_id = 0, instance_generation = 0;
  std::string property_name, semantic;
  Bytes bytes;
  bool partial = false;
  std::vector<std::string> warnings;
};
struct CropOptions {
  std::vector<ChannelBootstrap> channel_bootstrap;
  // Explicit opt-in; warning provenance is persisted as wire ErrorMessage(2).
  bool allow_partial_analysis = false;
};
// Exact [begin,end) source-event slice after a synthetic initial-state
// bootstrap. Both endpoints must be transaction boundaries. Analysis channels
// that require an unimplemented dictionary/scope bootstrap raise
// CropUnsupported.
PVD_CORE_API CroppedStream crop_events(const Session &,
                                       std::size_t begin_event_count,
                                       std::size_t end_event_count,
                                       const CropOptions & = {});
// Inclusive first/last global native-frame indices; keeps the first frame's
// BeginSection and includes the last EndSection. Source event ordering remains.
PVD_CORE_API CroppedStream crop_frames(const Session &, std::size_t first_frame,
                                       std::size_t last_frame,
                                       const CropOptions & = {});
PVD_CORE_API WriteResult save_crop(const std::string &utf8_path,
                                   const CroppedStream &,
                                   const WriteOptions & = {},
                                   bool overwrite = false);
} // namespace pvd::writing
