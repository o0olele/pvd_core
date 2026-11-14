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
    // Empty disables recording. A nonempty path is created exclusively, never overwritten.
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
