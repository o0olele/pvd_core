#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "pvd/capture.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <exception>
#include <filesystem>
#include <mutex>
#include <limits>
#include <type_traits>
#include <system_error>
#include <thread>

namespace pvd {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
int socket_error() { return WSAGetLastError(); }
bool retryable(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
void close_socket(Socket s) { if (s != invalid_socket) closesocket(s); }
struct SocketRuntime {
    SocketRuntime() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error) throw std::runtime_error("WSAStartup failed: " + std::to_string(error));
    }
    ~SocketRuntime() { WSACleanup(); }
};
void nonblocking(Socket s) {
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode))
        throw std::runtime_error("ioctlsocket failed: " + std::to_string(socket_error()));
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
int socket_error() { return errno; }
bool retryable(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
void close_socket(Socket s) { if (s != invalid_socket) ::close(s); }
struct SocketRuntime {};
void nonblocking(Socket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::system_error(errno, std::generic_category(), "fcntl nonblocking");
}
#endif

struct SocketOwner {
    Socket value = invalid_socket;
    SocketOwner() = default;
    explicit SocketOwner(Socket socket) : value(socket) {}
    ~SocketOwner() { reset(); }
    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;
    void reset(Socket next = invalid_socket) { close_socket(value); value = next; }
};

bool readable(Socket socket, std::chrono::milliseconds interval) {
#ifndef _WIN32
    if (socket >= FD_SETSIZE) throw std::runtime_error("socket exceeds select FD_SETSIZE");
#endif
    fd_set readset;
    FD_ZERO(&readset);
    FD_SET(socket, &readset);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(interval.count() / 1000);
    timeout.tv_usec = static_cast<long>((interval.count() % 1000) * 1000);
#ifdef _WIN32
    const int result = select(0, &readset, nullptr, nullptr, &timeout);
#else
    const int result = select(socket + 1, &readset, nullptr, nullptr, &timeout);
#endif
    if (result < 0) {
        const int error = socket_error();
        if (retryable(error)) return false;
        throw std::runtime_error("socket select failed: " + std::to_string(error));
    }
    return result > 0;
}

struct Recording {
    int fd = -1;
    ~Recording() { close(); }
    void create(const std::string& path) {
        if (path.empty()) return;
#ifdef _WIN32
        const auto error = _wsopen_s(&fd, std::filesystem::u8path(path).c_str(),
            _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
        if (error) { fd = -1; errno = error; }
#else
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "exclusive recording create: " + path);
    }
    void close() noexcept {
        if (fd < 0) return;
#ifdef _WIN32
        _close(fd);
#else
        ::close(fd);
#endif
        fd = -1;
    }
    void rollback(std::uint64_t offset) {
#ifdef _WIN32
        if (_chsize_s(fd, offset) || _lseeki64(fd, static_cast<__int64>(offset), SEEK_SET) < 0)
#else
        if (ftruncate(fd, static_cast<off_t>(offset)) || lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
#endif
            throw std::runtime_error("recording write failed and truncation to its previous safe prefix failed");
    }
    void write(const std::uint8_t* data, std::size_t size, std::uint64_t previous_size) {
        if (fd < 0) return;
        std::size_t done = 0;
        while (done < size) {
            const auto count = static_cast<unsigned>(std::min<std::size_t>(size - done, INT_MAX));
#ifdef _WIN32
            const auto written = _write(fd, data + done, count);
#else
            const auto written = ::write(fd, data + done, count);
#endif
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                const int error = errno;
                rollback(previous_size);
                throw std::system_error(error, std::generic_category(), "recording write (rolled back incomplete group)");
            }
            done += static_cast<std::size_t>(written);
        }
    }
};

std::size_t event_payload_bytes(const Event &event) {
    std::size_t size = sizeof(Event);
    auto add = [&](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size)
            throw std::overflow_error("capture poll payload size overflow");
        size += amount;
    };
    std::visit(
        [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, SetPropertyValue> || std::is_same_v<T, AppendPropertyValueData> ||
                          std::is_same_v<T, SetPropertyMessage> || std::is_same_v<T, SendPropertyMessageFromGroup>)
                add(v.data.size());
            else if constexpr (std::is_same_v<T, StringHandleEvent>)
                add(v.string.size());
            else if constexpr (std::is_same_v<T, SetCamera> || std::is_same_v<T, AddProfileZone> ||
                               std::is_same_v<T, AddProfileZoneEvent> || std::is_same_v<T, StreamEndEvent>)
                add(v.name.size());
            else if constexpr (std::is_same_v<T, ErrorMessage>) {
                add(v.message.size());
                add(v.file.size());
            } else if constexpr (std::is_same_v<T, CreateProperty>)
                add(v.values.size() * sizeof(NameHandleValue));
            else if constexpr (std::is_same_v<T, CreatePropertyMessage>)
                add(v.message_entries.size() * sizeof(StreamPropMessageArg));
        },
        event.payload);
    return size;
}
struct Truncated : std::runtime_error { using std::runtime_error::runtime_error; };
void error_text(std::string& target, const char* text) noexcept {
    try {
        std::size_t length = 0;
        while (text[length] && length < 2048) ++length;
        target.assign(text, length);
    } catch (...) {
        try { target = "capture error"; } catch (...) {}
    }
}
} // namespace

const char* capture_state_name(CaptureState state) noexcept {
    switch (state) {
    case CaptureState::idle: return "idle";
    case CaptureState::listening: return "listening";
    case CaptureState::receiving: return "receiving";
    case CaptureState::completed: return "completed";
    case CaptureState::cancelled: return "cancelled";
    case CaptureState::truncated: return "truncated";
    case CaptureState::failed: return "failed";
    }
    return "failed";
}

struct TcpCapture::Impl {
    explicit Impl(CaptureOptions settings) : options(std::move(settings)) {
        progress.address = options.address;
        progress.port = options.port;
    }
    CaptureOptions options;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::atomic<bool> cancelled{false};
    bool started = false;
    std::unique_ptr<SocketRuntime> runtime;
    SocketOwner listener;
    Recording recording;
    std::thread worker;
    CaptureStatus progress;
    std::unique_ptr<Session> session;

    void open_listener() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_NUMERICHOST;
        addrinfo* result = nullptr;
        const auto service = std::to_string(options.port);
        const int resolution = getaddrinfo(options.address.c_str(), service.c_str(), &hints, &result);
        if (resolution) throw std::runtime_error("invalid numeric bind address: " + options.address +
                                                 " (getaddrinfo " + std::to_string(resolution) + ")");
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(result, freeaddrinfo);
        int last_error = 0;
        for (auto* address = result; address; address = address->ai_next) {
            listener.reset(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
            if (listener.value == invalid_socket) { last_error = socket_error(); continue; }
            const int yes = 1;
#ifdef _WIN32
            if (setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                           reinterpret_cast<const char*>(&yes), sizeof(yes))) {
#else
            if (setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))) {
#endif
                last_error = socket_error(); continue;
            }
            if (::bind(listener.value, address->ai_addr, static_cast<int>(address->ai_addrlen)) ||
                ::listen(listener.value, 1)) { last_error = socket_error(); continue; }
            nonblocking(listener.value);
            sockaddr_storage bound{};
#ifdef _WIN32
            int size = sizeof(bound);
#else
            socklen_t size = sizeof(bound);
#endif
            if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&bound), &size))
                throw std::runtime_error("getsockname failed: " + std::to_string(socket_error()));
            progress.port = ntohs(bound.ss_family == AF_INET ? reinterpret_cast<sockaddr_in*>(&bound)->sin_port
                                                            : reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
            return;
        }
        listener.reset();
        throw std::runtime_error("bind/listen failed for " + options.address + ":" + service +
                                 " (socket error " + std::to_string(last_error) + ")");
    }

    void refresh_locked() {
        progress.header_complete = bool(session);
        if (session) {
            progress.event_count = session->event_count();
            if (session->state_available()) progress.frame_count = session->frames().size();
        }
    }

    // Called by the worker with mutex held. If a write fails, Recording rolls
    // back to the preceding safe prefix, so no partial group is advertised.
    void commit_raw(std::uint64_t end, Bytes& pending, std::uint64_t& offset, std::size_t event_count) {
        if (end < offset || end - offset > pending.size())
            throw std::logic_error("decoder group boundary is outside the pending raw prefix");
        const auto length = static_cast<std::size_t>(end - offset);
        recording.write(pending.data(), length, progress.recorded_bytes);
        if (recording.fd >= 0) progress.recorded_bytes += length;
        progress.recoverable_bytes = end;
        progress.recoverable_event_count = event_count;
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(length));
        offset = end;
    }

    void consume(StreamDecoder& decoder, Bytes& pending, std::uint64_t& pending_offset,
                 std::size_t& applied_events) {
        auto events = decoder.take_events();
        auto boundaries = decoder.take_group_boundaries();
        std::lock_guard<std::mutex> lock(mutex);
        if (!session && decoder.header()) {
            session = std::make_unique<Session>(*decoder.header(), options.session_options);
            commit_raw(28, pending, pending_offset, 0);
        }
        std::size_t cursor = 0;
        for (const auto& boundary : boundaries) {
            if (!session || boundary.event_count < applied_events ||
                boundary.event_count - applied_events > events.size() - cursor)
                throw std::logic_error("decoder event and group boundaries are inconsistent");
            const auto count = boundary.event_count - applied_events;
            std::vector<Event> group;
            group.reserve(count);
            for (std::size_t i = 0; i < count; ++i) group.push_back(std::move(events[cursor++]));
            session->append(std::move(group));
            applied_events = boundary.event_count;
            if (session->at_transaction_boundary())
                commit_raw(boundary.end_offset, pending, pending_offset, boundary.event_count);
        }
        if (cursor != events.size()) throw std::logic_error("decoded events have no complete group boundary");
        refresh_locked();
    }

    void run() noexcept {
        SocketOwner server;
        // Ownership is transferred once; cancel() only sets an atomic flag.
        server.value = listener.value;
        listener.value = invalid_socket;
        SocketOwner client;
        Bytes pending;
        std::uint64_t pending_offset = 0;
        std::size_t applied_events = 0;
        CaptureState final_state = CaptureState::completed;
        std::string error;
        try {
            StreamDecoder decoder(options.session_options.decode_limits);
            while (!cancelled.load() && client.value == invalid_socket) {
                if (!readable(server.value, options.poll_interval)) continue;
                client.value = ::accept(server.value, nullptr, nullptr);
                if (client.value == invalid_socket) {
                    const int code = socket_error();
                    if (retryable(code)) continue;
                    throw std::runtime_error("accept failed: " + std::to_string(code));
                }
                nonblocking(client.value);
            }
            server.reset(); // Do not accept or append another connection.
            if (!cancelled.load()) {
                std::lock_guard<std::mutex> lock(mutex);
                progress.state = CaptureState::receiving;
            }
            Bytes input(std::min(options.read_buffer_bytes, options.max_pending_record_bytes));
            bool eof = false;
            while (!cancelled.load() && !eof) {
                if (!readable(client.value, options.poll_interval)) continue;
                const auto capacity = options.max_pending_record_bytes - pending.size();
                const auto count = std::min(input.size(), capacity);
                // At the bound, peek permits EOF/cancellation to retain their
                // proper status without retaining one byte over the limit.
                char probe = 0;
                const int received = ::recv(client.value, count ? reinterpret_cast<char*>(input.data()) : &probe,
                                             static_cast<int>(count ? count : 1), count ? 0 : MSG_PEEK);
                if (received == 0) { eof = true; break; }
                if (received < 0) {
                    const int code = socket_error();
                    if (retryable(code)) continue;
                    throw Truncated("connection receive failed: " + std::to_string(code));
                }
                if (!count) throw std::runtime_error("max_pending_record_bytes exceeded before a recoverable transaction boundary");
                pending.insert(pending.end(), input.begin(), input.begin() + received);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    progress.wire_bytes += static_cast<unsigned>(received);
                }
                std::exception_ptr parse_error;
                try { decoder.feed(input.data(), static_cast<std::size_t>(received)); }
                catch (...) { parse_error = std::current_exception(); }
                // A feed may have validated earlier groups before rejecting a
                // later one. Drain them before reporting the malformed tail.
                consume(decoder, pending, pending_offset, applied_events);
                if (parse_error) std::rethrow_exception(parse_error);
            }
            if (cancelled.load()) {
                final_state = CaptureState::cancelled;
            } else {
                try {
                    decoder.finish();
                    std::lock_guard<std::mutex> lock(mutex);
                    if (session) session->finish();
                } catch (const std::exception& exception) {
                    final_state = CaptureState::truncated;
                    error_text(error, exception.what());
                }
            }
        } catch (const Truncated& exception) {
            final_state = cancelled.load() ? CaptureState::cancelled : CaptureState::truncated;
            error_text(error, exception.what());
        } catch (const std::exception& exception) {
            final_state = cancelled.load() ? CaptureState::cancelled : CaptureState::failed;
            error_text(error, exception.what());
        } catch (...) {
            final_state = CaptureState::failed;
            error_text(error, "unknown capture worker failure");
        }
        client.reset();
        server.reset();
        recording.close();
        // Session can deliberately retain only its event log if a persistent
        // allocation failure prevents rebuilding state indexes. Terminal status
        // reporting must not let that failure escape this noexcept worker.
        try {
            std::lock_guard<std::mutex> lock(mutex);
            refresh_locked();
            progress.discarded_tail_bytes = progress.wire_bytes - progress.recoverable_bytes;
            progress.state = final_state;
            if (!error.empty()) {
                try { progress.errors.push_back(std::move(error)); } catch (...) {}
            }
        } catch (...) {
            // A valid std::mutex does not allocate while locking. This is a last
            // containment boundary for exceptional platform/runtime failures.
        }
    }
};

TcpCapture::TcpCapture(CaptureOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
TcpCapture::~TcpCapture() { cancel(); wait(); }

void TcpCapture::start() {
    std::lock_guard<std::mutex> life(impl_->lifecycle);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started) throw std::logic_error("TcpCapture can only be started once");
    impl_->started = true;
    try {
        const auto& options = impl_->options;
        if (options.address.empty()) throw std::invalid_argument("capture bind address must not be empty");
        if (!options.read_buffer_bytes || options.read_buffer_bytes > INT_MAX)
            throw std::invalid_argument("read_buffer_bytes must be in [1, INT_MAX]");
        if (options.max_pending_record_bytes < 28)
            throw std::invalid_argument("max_pending_record_bytes must be at least the 28-byte stream header");
        if (options.poll_interval.count() < 1 || options.poll_interval.count() > 1000)
            throw std::invalid_argument("poll_interval must be in [1ms, 1000ms]");
        [[maybe_unused]] StreamDecoder validate_limits(options.session_options.decode_limits);
        impl_->progress.errors.reserve(1);
        impl_->runtime = std::make_unique<SocketRuntime>();
        impl_->open_listener();
        impl_->recording.create(options.recording_path);
        impl_->cancelled.store(false);
        impl_->progress.state = CaptureState::listening;
        impl_->worker = std::thread([self = impl_.get()] { self->run(); });
    } catch (const std::exception& error) {
        impl_->listener.reset();
        impl_->recording.close();
        impl_->progress.state = CaptureState::failed;
        impl_->progress.errors.push_back(error.what());
        throw;
    }
}
void TcpCapture::cancel() noexcept { impl_->cancelled.store(true); }
void TcpCapture::wait() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle);
    if (impl_->worker.joinable()) impl_->worker.join();
}
CaptureStatus TcpCapture::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->progress;
}
namespace {
CapturePoll copy_poll(const Session* session, const CaptureStatus& progress, std::size_t cursor,
                      std::size_t max_events, std::size_t max_payload_bytes) {
    CapturePoll out;
    out.status = progress;
    out.begin_event_count = out.end_event_count = cursor;
    if (cursor > out.status.recoverable_event_count)
        throw std::out_of_range("capture poll cursor exceeds recoverable event prefix");
    if (!session) {
        if (cursor)
            throw std::out_of_range("capture has no event source");
        return out;
    }
    out.header = session->header();
    const auto &events = session->events();
    if (out.status.recoverable_event_count > events.size())
        throw std::logic_error("recoverable event prefix exceeds accepted source");
    const auto available = out.status.recoverable_event_count - cursor;
    const auto count = std::min(max_events, available);
    for (std::size_t i = 0; i < count; ++i) {
        const auto cost = event_payload_bytes(events[cursor + i]);
        if (i && cost > max_payload_bytes - std::min(max_payload_bytes, out.copied_payload_bytes))
            break;
        if (!i && cost > max_payload_bytes)
            out.oversized_event = true;
        if (cost > std::numeric_limits<std::size_t>::max() - out.copied_payload_bytes)
            throw std::overflow_error("capture poll payload size overflow");
        out.copied_payload_bytes += cost;
        ++out.end_event_count;
        if (out.copied_payload_bytes >= max_payload_bytes)
            break;
    }
    out.events.assign(events.begin() + static_cast<std::ptrdiff_t>(cursor),
                      events.begin() + static_cast<std::ptrdiff_t>(out.end_event_count));
    return out;
}
} // namespace
CapturePoll TcpCapture::poll_from(std::size_t cursor, std::size_t max_events, std::size_t max_payload_bytes) const {
    if (!max_events || !max_payload_bytes)
        throw std::invalid_argument("capture poll limits must be nonzero");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return copy_poll(impl_->session.get(), impl_->progress, cursor, max_events, max_payload_bytes);
}
std::optional<CapturePoll> TcpCapture::try_poll_from(std::size_t cursor, std::size_t max_events,
                                                  std::size_t max_payload_bytes) const {
    if (!max_events || !max_payload_bytes)
        throw std::invalid_argument("capture poll limits must be nonzero");
    std::unique_lock<std::mutex> lock(impl_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    return copy_poll(impl_->session.get(), impl_->progress, cursor, max_events, max_payload_bytes);
}
std::optional<StreamHeader> TcpCapture::header() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session ? std::optional<StreamHeader>(impl_->session->header()) : std::nullopt;
}
Metadata TcpCapture::metadata() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session ? impl_->session->metadata() : Metadata{};
}
std::vector<Event> TcpCapture::events() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session ? impl_->session->events() : std::vector<Event>{};
}
std::vector<FrameMarker> TcpCapture::frames() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session ? impl_->session->frames() : std::vector<FrameMarker>{};
}
std::vector<SectionMarker> TcpCapture::sections() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session ? impl_->session->sections() : std::vector<SectionMarker>{};
}
std::vector<ObjectLifetime> TcpCapture::lifetimes() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session ? impl_->session->lifetimes() : std::vector<ObjectLifetime>{};
}
std::vector<ChannelPacket> TcpCapture::channels() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session ? impl_->session->channels() : std::vector<ChannelPacket>{};
}
Snapshot TcpCapture::latest() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->session ? impl_->session->latest() : Snapshot{};
}
Snapshot TcpCapture::snapshot_at_event(std::size_t count) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->session) throw std::out_of_range("capture has no stream header yet");
    return impl_->session->snapshot_at_event(count);
}
Snapshot TcpCapture::snapshot_at_frame(std::size_t frame) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->session) throw std::out_of_range("capture has no stream header yet");
    return impl_->session->snapshot_at_frame(frame);
}
Snapshot TcpCapture::snapshot_at_frame(std::uint64_t stream, std::uint64_t scene, std::size_t frame) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->session) throw std::out_of_range("capture has no stream header yet");
    return impl_->session->snapshot_at_frame(stream, scene, frame);
}
} // namespace pvd
