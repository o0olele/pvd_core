#pragma once

#include "pvd/capture_export.hpp"
#include "pvd/session.hpp"
#include <chrono>
#include <memory>

namespace pvd {

enum class CaptureState { idle, listening, receiving, completed, cancelled, truncated, failed };
PVD_TRANSPORT_API const char* capture_state_name(CaptureState state) noexcept;

struct CaptureOptions {
    // A numeric IPv4 or IPv6 bind address. Use port 0 to request an ephemeral port.
    std::string address = "127.0.0.1";
    std::uint16_t port = 5425;
    SessionOptions session_options;
    std::size_t read_buffer_bytes = 64u * 1024u;
    std::chrono::milliseconds poll_interval{50};
    // UTF-8 path. Empty disables recording; nonempty is created exclusively, never overwritten.
    std::string recording_path;
    // Bounds the raw tail since the last complete group with no pending property transaction.
    std::size_t max_pending_record_bytes = 128u * 1024u * 1024u;
};

struct CaptureStatus {
    CaptureState state = CaptureState::idle;
    std::string address;
    std::uint16_t port = 0;
    std::uint64_t wire_bytes = 0;
    std::uint64_t recorded_bytes = 0;
    std::uint64_t recoverable_bytes = 0;
    std::uint64_t discarded_tail_bytes = 0;
    std::size_t event_count = 0, frame_count = 0;
    std::size_t recoverable_event_count = 0;
    bool header_complete = false;
    std::vector<std::string> errors;
    bool terminal() const noexcept {
        return state == CaptureState::completed || state == CaptureState::cancelled ||
               state == CaptureState::truncated || state == CaptureState::failed;
    }
};

// Coherent cursor read under the same transport lock as status publication.
// events covers [begin_event_count,end_event_count), bounded above by the
// recoverable transaction prefix. A batch can cut a property transaction; the
// consumer keeps it pending until subsequent batches close it. No old events
// before the cursor are copied. The first event may exceed the byte hint so a
// single valid event cannot stall progress (bounded by decoder group/blob
// limits).
struct CapturePoll {
    std::optional<StreamHeader> header;
    CaptureStatus status;
    std::size_t begin_event_count = 0, end_event_count = 0;
    std::size_t copied_payload_bytes = 0;
    bool oversized_event = false;
    std::vector<Event> events;
    bool caught_up() const noexcept { return end_event_count == status.recoverable_event_count; }
};
// One listener, one accepted connection, one Session. Construct a new object to
// receive another connection. Queries and cancel() may run concurrently with the
// worker; all query results are independent copies (payload bytes are immutable).
// Incoming data is decoded/replayed/recorded inline, with TCP backpressure and no
// producer queue. Session history is subject to SessionOptions::decode_limits.
class TcpCapture {
public:
    explicit PVD_TRANSPORT_API TcpCapture(CaptureOptions options = {});
    PVD_TRANSPORT_API ~TcpCapture();
    TcpCapture(const TcpCapture&) = delete;
    TcpCapture& operator=(const TcpCapture&) = delete;
    TcpCapture(TcpCapture&&) = delete;
    TcpCapture& operator=(TcpCapture&&) = delete;

    // Synchronously bind/listen and exclusively create the recording before
    // returning. Throws on setup failure; status() also records that failure.
    PVD_TRANSPORT_API void start();
    // Cooperatively cancel accept/recv within poll_interval plus current inline
    // processing. The caller never closes a socket owned by the worker.
    PVD_TRANSPORT_API void cancel() noexcept;
    PVD_TRANSPORT_API void wait();

    PVD_TRANSPORT_API CaptureStatus status() const;
    PVD_TRANSPORT_API CapturePoll poll_from(std::size_t accepted_event_count,
        std::size_t max_events = 16384, std::size_t max_payload_bytes = 8u * 1024u * 1024u) const;
    // Same coherent cursor/limits as poll_from. Returns nullopt immediately
    // while receive/decode owns the source lock; no cursor is consumed.
    PVD_TRANSPORT_API std::optional<CapturePoll> try_poll_from(std::size_t accepted_event_count,
        std::size_t max_events = 16384, std::size_t max_payload_bytes = 8u * 1024u * 1024u) const;
    PVD_TRANSPORT_API std::optional<StreamHeader> header() const;
    PVD_TRANSPORT_API Metadata metadata() const;
    PVD_TRANSPORT_API std::vector<Event> events() const;
    PVD_TRANSPORT_API std::vector<FrameMarker> frames() const;
    PVD_TRANSPORT_API std::vector<SectionMarker> sections() const;
    PVD_TRANSPORT_API std::vector<ObjectLifetime> lifetimes() const;
    PVD_TRANSPORT_API std::vector<ChannelPacket> channels() const;
    PVD_TRANSPORT_API Snapshot latest() const;
    PVD_TRANSPORT_API Snapshot snapshot_at_event(std::size_t event_count) const;
    PVD_TRANSPORT_API Snapshot snapshot_at_frame(std::size_t global_frame_index) const;
    PVD_TRANSPORT_API Snapshot snapshot_at_frame(std::uint64_t stream_id, std::uint64_t scene_id,
                                                std::size_t frame_index) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pvd
