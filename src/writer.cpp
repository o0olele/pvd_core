#include "pvd/writer.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <ostream>
#include <set>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pvd::writing {
namespace {
constexpr auto u32max = std::numeric_limits<std::uint32_t>::max();
struct Encoder {
  ByteOrder order;
  std::size_t limit;
  Bytes bytes;
  void room(std::size_t n) {
    if (n > limit - bytes.size())
      throw std::length_error("encoded event exceeds writer byte budget");
  }
  void u(std::uint64_t value, std::size_t width) {
    room(width);
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          value >> ((order == ByteOrder::little ? i : width - 1 - i) * 8)));
  }
  void f(float value) {
    std::uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, 4);
    u(bits, 4);
  }
  void vec(Vec3 value) {
    f(value.x);
    f(value.y);
    f(value.z);
  }
  void name(WireName value) {
    u(value.namespace_handle, 4);
    u(value.name_handle, 4);
  }
  void count(std::size_t value) {
    if (value > u32max)
      throw std::length_error("wire count exceeds uint32");
    u(value, 4);
  }
  void string(const std::string &value) {
    if (value.find('\0') != std::string::npos)
      throw std::invalid_argument("wire string contains embedded NUL");
    if (value.size() >= u32max)
      throw std::length_error("wire string exceeds uint32 length");
    count(value.empty() ? 0 : value.size() + 1);
    room(value.size() + (value.empty() ? 0 : 1));
    bytes.insert(bytes.end(), value.begin(), value.end());
    if (!value.empty())
      u(0, 1);
  }
  void blob(const Bytes &value) {
    count(value.size());
    room(value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
};
void valid_options(const WriteOptions &o) {
  if (!o.target_group_bytes || !o.max_group_bytes ||
      o.target_group_bytes > o.max_group_bytes || o.max_group_bytes > u32max ||
      !o.max_events_per_group || o.max_events_per_group > u32max ||
      o.max_group_bytes > std::numeric_limits<std::size_t>::max() - 24)
    throw std::invalid_argument("invalid writer group budgets");
}
bool cancelled(const WriteOptions &o) { return o.cancelled && o.cancelled(); }
template <class Sink>
WriteResult write(Sink &&sink, const StreamHeader &header,
                  const std::vector<Event> &events, std::size_t count,
                  const WriteOptions &options) {
  valid_options(options);
  WriteResult result;
  result.requested_event_count = count;
  auto emit = [&](const Bytes &bytes) {
    if (bytes.size() > options.max_output_bytes - result.bytes_written)
      throw std::length_error("writer output byte budget exceeded");
    sink(bytes);
    result.bytes_written += bytes.size();
  };
  auto stop = [&] {
    if (!cancelled(options))
      return false;
    result.state = WriteState::cancelled;
    return true;
  };
  if (stop())
    return result;
  emit(encode_header(header));
  Bytes body;
  std::size_t body_events = 0;
  std::uint64_t stream = 0, timestamp = 0;
  auto flush = [&] {
    if (!body_events)
      return;
    Encoder group{header.byte_order, 24 + options.max_group_bytes, {}};
    group.count(body.size());
    group.count(body_events);
    group.u(stream, 8);
    group.u(timestamp, 8);
    group.room(body.size());
    group.bytes.insert(group.bytes.end(), body.begin(), body.end());
    emit(group.bytes);
    result.event_count += body_events;
    ++result.group_count;
    body.clear();
    body_events = 0;
  };
  for (std::size_t i = 0; i < count; ++i) {
    if (stop())
      return result;
    const auto &event = events[i];
    auto encoded =
        encode_event(event, header.byte_order, options.max_group_bytes);
    if (body_events &&
        (event.stream_id != stream || event.timestamp != timestamp ||
         body_events >= options.max_events_per_group ||
         encoded.size() > options.max_group_bytes - body.size() ||
         body.size() >= options.target_group_bytes ||
         encoded.size() >
             options.target_group_bytes -
                 std::min(body.size(), options.target_group_bytes))) {
      if (stop())
        return result;
      flush();
    }
    if (!body_events) {
      stream = event.stream_id;
      timestamp = event.timestamp;
    }
    body.insert(body.end(), encoded.begin(), encoded.end());
    ++body_events;
  }
  if (stop())
    return result;
  flush();
  stop(); // Cancellation during the final blocking sink write still wins.
  return result;
}
struct Transactions {
  std::set<std::uint64_t> streams;
  void apply(const Event &e) {
    if (std::holds_alternative<BeginSetPropertyValue>(e.payload) ||
        std::holds_alternative<BeginPropertyMessageGroup>(e.payload)) {
      if (!streams.insert(e.stream_id).second)
        throw std::invalid_argument("source has nested property transaction");
    } else if (std::holds_alternative<EndSetPropertyValue>(e.payload) ||
               std::holds_alternative<EndPropertyMessageGroup>(e.payload)) {
      if (!streams.erase(e.stream_id))
        throw std::invalid_argument(
            "source has unmatched property transaction end");
    }
  }
};
struct Temporary {
  std::filesystem::path path;
  int fd = -1;
  bool published = false;
  explicit Temporary(const std::filesystem::path &destination, bool overwrite) {
    if (destination.empty() || destination.filename().empty())
      throw std::invalid_argument("empty save destination filename");
    if (!overwrite && std::filesystem::exists(destination))
      throw std::system_error(std::make_error_code(std::errc::file_exists),
                              "save destination exists");
    static std::atomic<std::uint64_t> serial{0};
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
      const auto suffix =
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(++serial);
      path = destination;
      path += std::filesystem::path(".pvd-tmp-" + suffix);
#ifdef _WIN32
      const auto error = _wsopen_s(&fd, path.c_str(),
                                   _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                                   _SH_DENYNO, _S_IREAD | _S_IWRITE);
      if (error) {
        fd = -1;
        errno = error;
      }
#else
      fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
      if (fd >= 0)
        return;
      if (errno != EEXIST)
        throw std::system_error(errno, std::generic_category(),
                                "create private save file");
    }
    throw std::runtime_error("cannot allocate unique private save filename");
  }
  ~Temporary() {
    close_noexcept();
    if (!published) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
  void close_noexcept() noexcept {
    if (fd < 0)
      return;
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
    fd = -1;
  }
  void put(const Bytes &data) {
    std::size_t at = 0;
    while (at < data.size()) {
      const auto amount = static_cast<unsigned>(
          std::min<std::size_t>(INT_MAX, data.size() - at));
#ifdef _WIN32
      const auto n = _write(fd, data.data() + at, amount);
#else
      const auto n = ::write(fd, data.data() + at, amount);
#endif
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw std::system_error(errno ? errno : EIO, std::generic_category(),
                                "write private save file");
      at += static_cast<std::size_t>(n);
    }
  }
  void publish(const std::filesystem::path &destination, bool overwrite) {
#ifdef _WIN32
    if (_commit(fd))
      throw std::system_error(errno, std::generic_category(),
                              "flush private save file");
    const auto closing = fd;
    fd = -1;
    if (_close(closing))
      throw std::system_error(errno, std::generic_category(),
                              "close private save file");
    if (!MoveFileExW(path.c_str(), destination.c_str(),
                     MOVEFILE_WRITE_THROUGH |
                         (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(), "publish saved capture");
#else
    if (::fsync(fd))
      throw std::system_error(errno, std::generic_category(),
                              "flush private save file");
    const auto closing = fd;
    fd = -1;
    if (::close(closing))
      throw std::system_error(errno, std::generic_category(),
                              "close private save file");
    if (overwrite) {
      if (::rename(path.c_str(), destination.c_str()))
        throw std::system_error(errno, std::generic_category(),
                                "publish saved capture");
    } else {
      if (::link(path.c_str(), destination.c_str()))
        throw std::system_error(errno, std::generic_category(),
                                "publish saved capture without overwrite");
      ::unlink(path.c_str());
    }
#endif
    published = true;
  }
};
template <class Function>
WriteResult save(const std::string &utf8, bool overwrite, Function &&function) {
  if (utf8.find('\0') != std::string::npos)
    throw std::invalid_argument("save destination contains embedded NUL");
  const auto destination = std::filesystem::u8path(utf8);
  Temporary temporary(destination, overwrite);
  auto result = function([&](const Bytes &bytes) { temporary.put(bytes); });
  if (result.state == WriteState::completed)
    temporary.publish(destination, overwrite);
  return result;
}
} // namespace
Bytes encode_header(const StreamHeader &h) {
  if (h.stream_version != supported_stream_version ||
      !h.timestamp_denominator || (h.stream_flags & ~1u))
    throw std::invalid_argument("invalid/unsupported output stream header");
  Encoder w{h.byte_order, 28, {}};
  w.u(stream_magic, 4);
  w.u(h.stream_version, 4);
  w.u(h.timestamp_numerator, 8);
  w.u(h.timestamp_denominator, 8);
  w.u(h.stream_flags, 4);
  return std::move(w.bytes);
}
Bytes encode_event(const Event &e, ByteOrder order, std::size_t max_bytes) {
  Encoder w{order, max_bytes, {}};
  w.u(e.tag(), 1);
  switch (e.tag()) {
  case 1: {
    const auto &v = std::get<StringHandleEvent>(e.payload);
    w.string(v.string);
    w.u(v.handle, 4);
    break;
  }
  case 2:
    w.name(std::get<CreateClass>(e.payload).name);
    break;
  case 3: {
    const auto &v = std::get<DeriveClass>(e.payload);
    w.name(v.parent);
    w.name(v.child);
    break;
  }
  case 4: {
    const auto &v = std::get<CreateProperty>(e.payload);
    w.name(v.class_name);
    w.u(v.name, 4);
    w.u(v.semantic, 4);
    w.name(v.datatype_name);
    w.u(v.property_type, 1);
    w.count(v.values.size());
    for (const auto &p : v.values) {
      w.u(p.name, 4);
      w.u(p.value, 4);
    }
    break;
  }
  case 5: {
    const auto &v = std::get<CreatePropertyMessage>(e.payload);
    w.name(v.class_name);
    w.name(v.message_name);
    w.count(v.message_entries.size());
    for (const auto &p : v.message_entries) {
      w.u(p.property_name, 4);
      w.name(p.datatype_name);
      w.u(p.message_offset, 4);
      w.u(p.byte_size, 4);
    }
    w.u(v.message_byte_size, 4);
    break;
  }
  case 6: {
    const auto &v = std::get<CreateInstance>(e.payload);
    w.name(v.class_name);
    w.u(v.instance_id, 8);
    break;
  }
  case 7: {
    const auto &v = std::get<SetPropertyValue>(e.payload);
    w.u(v.instance_id, 8);
    w.u(v.property_name, 4);
    w.name(v.incoming_type_name);
    w.u(v.num_items, 4);
    w.blob(v.data);
    break;
  }
  case 8: {
    const auto &v = std::get<BeginSetPropertyValue>(e.payload);
    w.u(v.instance_id, 8);
    w.u(v.property_name, 4);
    w.name(v.incoming_type_name);
    break;
  }
  case 9: {
    const auto &v = std::get<AppendPropertyValueData>(e.payload);
    w.blob(v.data);
    w.u(v.num_items, 4);
    break;
  }
  case 10:
    break;
  case 11: {
    const auto &v = std::get<SetPropertyMessage>(e.payload);
    w.u(v.instance_id, 8);
    w.name(v.message_name);
    w.blob(v.data);
    break;
  }
  case 12:
    w.name(std::get<BeginPropertyMessageGroup>(e.payload).msg_name);
    break;
  case 13: {
    const auto &v = std::get<SendPropertyMessageFromGroup>(e.payload);
    w.u(v.instance, 8);
    w.blob(v.data);
    break;
  }
  case 14:
    break;
  case 15:
    w.u(std::get<DestroyInstance>(e.payload).instance_id, 8);
    break;
  case 16: {
    const auto &v = std::get<PushBackObjectRef>(e.payload);
    w.u(v.instance_id, 8);
    w.u(v.property, 4);
    w.u(v.object_ref, 8);
    break;
  }
  case 17: {
    const auto &v = std::get<RemoveObjectRef>(e.payload);
    w.u(v.instance_id, 8);
    w.u(v.property, 4);
    w.u(v.object_ref, 8);
    break;
  }
  case 18: {
    const auto &v = std::get<BeginSection>(e.payload);
    w.u(v.section_id, 8);
    w.u(v.name, 4);
    w.u(v.timestamp, 8);
    break;
  }
  case 19: {
    const auto &v = std::get<EndSection>(e.payload);
    w.u(v.section_id, 8);
    w.u(v.name, 4);
    w.u(v.timestamp, 8);
    break;
  }
  case 20: {
    const auto &v = std::get<SetPickable>(e.payload);
    w.u(v.instance_id, 8);
    w.u(v.pickable, 1);
    break;
  }
  case 21: {
    const auto &v = std::get<SetColor>(e.payload);
    w.u(v.instance_id, 8);
    w.u(v.color.r, 1);
    w.u(v.color.g, 1);
    w.u(v.color.b, 1);
    w.u(v.color.a, 1);
    break;
  }
  case 22: {
    const auto &v = std::get<SetIsTopLevel>(e.payload);
    w.u(v.instance_id, 8);
    w.u(v.is_top_level, 1);
    break;
  }
  case 23: {
    const auto &v = std::get<SetCamera>(e.payload);
    w.string(v.name);
    w.vec(v.position);
    w.vec(v.up);
    w.vec(v.target);
    break;
  }
  case 24: {
    const auto &v = std::get<AddProfileZone>(e.payload);
    w.u(v.instance_id, 8);
    w.string(v.name);
    break;
  }
  case 25: {
    const auto &v = std::get<AddProfileZoneEvent>(e.payload);
    w.u(v.instance_id, 8);
    w.string(v.name);
    w.u(v.event_id, 2);
    w.u(v.compile_time_enabled, 1);
    break;
  }
  case 26:
    w.string(std::get<StreamEndEvent>(e.payload).name);
    break;
  case 27: {
    const auto &v = std::get<ErrorMessage>(e.payload);
    w.u(v.code, 4);
    w.string(v.message);
    w.string(v.file);
    w.u(v.line, 4);
    break;
  }
  case 28: {
    const auto &v = std::get<OriginShift>(e.payload);
    w.u(v.instance_id, 8);
    w.vec(v.shift);
    break;
  }
  default:
    throw std::invalid_argument("invalid event alternative");
  }
  return std::move(w.bytes);
}
WriteResult write_raw(std::ostream &out, const StreamHeader &header,
                      const std::vector<Event> &events,
                      const WriteOptions &options) {
  return write(
      [&](const Bytes &bytes) {
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out)
          throw std::runtime_error("capture output stream write failed");
      },
      header, events, events.size(), options);
}
SafePrefix safe_prefix(const Session &session, std::size_t requested) {
  if (requested == std::numeric_limits<std::size_t>::max())
    requested = session.event_count();
  if (requested > session.event_count())
    throw std::out_of_range("save prefix exceeds Session event count");
  Transactions pending;
  std::size_t safe = 0;
  for (std::size_t i = 0; i < requested; ++i) {
    pending.apply(session.events()[i]);
    if (pending.streams.empty())
      safe = i + 1;
  }
  return {requested, safe, requested - safe};
}
WriteResult write_session(std::ostream &out, const Session &session,
                          const WriteOptions &options, std::size_t requested) {
  auto prefix = safe_prefix(session, requested);
  auto result = write(
      [&](const Bytes &bytes) {
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out)
          throw std::runtime_error("capture output stream write failed");
      },
      session.header(), session.events(), prefix.event_count, options);
  result.requested_event_count = prefix.requested_event_count;
  result.discarded_tail_events = prefix.discarded_tail_events;
  return result;
}
WriteResult save_session(const std::string &path, const Session &session,
                         const WriteOptions &options, bool overwrite,
                         std::size_t requested) {
  auto prefix = safe_prefix(session, requested);
  auto result = save(path, overwrite, [&](auto &&sink) {
    return write(sink, session.header(), session.events(), prefix.event_count,
                 options);
  });
  result.requested_event_count = prefix.requested_event_count;
  result.discarded_tail_events = prefix.discarded_tail_events;
  return result;
}
CroppedStream crop_events(const Session &session, std::size_t begin,
                          std::size_t end, const CropOptions &options) {
  if (begin > end || end > session.event_count())
    throw std::out_of_range("invalid crop source-event range");
  if (safe_prefix(session, begin).event_count != begin ||
      safe_prefix(session, end).event_count != end)
    throw CropUnsupported(
        "crop endpoints must be outside all property/message transactions");
  CroppedStream crop;
  crop.header = session.header();
  crop.source_begin_event_count = begin;
  crop.source_end_event_count = end;
  const auto &source = session.events();
  if (!begin) {
    crop.events.assign(source.begin(),
                       source.begin() + static_cast<std::ptrdiff_t>(end));
    return crop;
  }
  const auto snapshot = session.snapshot_at_event(begin);
  using ChannelKey = std::tuple<std::uint64_t, std::uint64_t, std::string>;
  std::map<ChannelKey, std::string> required;
  for (const auto &channel : session.channels())
    if (channel.event_index < begin && snapshot.instance(channel.instance_id) &&
        (channel.semantic == "profile event stream" ||
         channel.semantic == "memory event stream"))
      required[{channel.stream_id, channel.instance_id,
                channel.property_name}] = channel.semantic;
  std::map<ChannelKey, const ChannelBootstrap *> provided;
  for (const auto &channel : options.channel_bootstrap) {
    if (channel.partial && !options.allow_partial_analysis)
      throw CropUnsupported("partial analysis bootstrap requires explicit "
                            "opt-in; full save retains original diagnostics");
    const ChannelKey key{channel.stream_id, channel.instance_id,
                         channel.property_name};
    const auto found = required.find(key);
    const auto *instance = snapshot.instance(channel.instance_id);
    if (found == required.end() || found->second != channel.semantic ||
        !instance || instance->generation != channel.instance_generation ||
        !provided.emplace(key, &channel).second)
      throw CropUnsupported(
          "analysis channel bootstrap source/semantic/generation is invalid or "
          "duplicated");
    if (channel.bytes.size() > u32max)
      throw CropUnsupported(
          "analysis channel bootstrap exceeds uint32 item count");
  }
  if (provided.size() != required.size())
    throw CropUnsupported(
        "range crop requires complete profile scope or memory "
        "dictionary/active-allocation bootstrap for every live source; use "
        "full save or a zero-start prefix");
  std::set<std::uint64_t> used_streams;
  for (const auto &event : source)
    used_streams.insert(event.stream_id);
  std::uint64_t candidate = std::numeric_limits<std::uint64_t>::max();
  auto next_stream = [&] {
    while (used_streams.count(candidate)) {
      if (!candidate)
        throw CropUnsupported("no free synthetic stream ID for crop bootstrap");
      --candidate;
    }
    const auto result = candidate;
    used_streams.insert(result);
    return result;
  };
  const auto bootstrap_stream = next_stream();
  auto emit = [&](std::uint64_t stream, Payload payload) {
    crop.events.push_back({stream, 0, 0, std::move(payload)});
  };
  for (const auto &channel : options.channel_bootstrap)
    if (channel.partial) {
      const auto message = "Partial crop analysis baseline: source stream " +
                           std::to_string(channel.stream_id) + ", buffer " +
                           std::to_string(channel.instance_id) +
                           ". Earlier incomplete memory/profile history was "
                           "excluded; totals are relative to this baseline.";
      crop.warnings.push_back(message);
      emit(bootstrap_stream, ErrorMessage{2, message, "pvd_core/crop", 0});
      for (const auto &warning : channel.warnings) {
        crop.warnings.push_back(warning);
        emit(bootstrap_stream,
             ErrorMessage{2, "Crop source diagnostic: " + warning,
                          "pvd_core/crop", 0});
      }
    }
  std::map<std::uint64_t, std::map<std::uint32_t, std::string>>
      original_handles;
  using SectionKey = std::tuple<std::uint64_t, std::uint64_t, std::string>;
  std::map<SectionKey, Event> open_sections;
  // Preserve source handle contexts verbatim, as opaque message/property data
  // may contain StringHandle values. Metadata is not a motion-history replay.
  for (std::size_t i = 0; i < begin; ++i) {
    const auto &e = source[i];
    if (const auto *text = std::get_if<StringHandleEvent>(&e.payload)) {
      original_handles[e.stream_id][text->handle] = text->string;
      crop.events.push_back(e);
    } else if (std::holds_alternative<CreateClass>(e.payload) ||
               std::holds_alternative<DeriveClass>(e.payload) ||
               std::holds_alternative<CreateProperty>(e.payload) ||
               std::holds_alternative<CreatePropertyMessage>(e.payload))
      crop.events.push_back(e);
    else if (const auto *section_begin = std::get_if<BeginSection>(&e.payload))
      open_sections[{e.stream_id, section_begin->section_id,
                     original_handles[e.stream_id][section_begin->name]}] = e;
    else if (const auto *section_end = std::get_if<EndSection>(&e.payload))
      open_sections.erase({e.stream_id, section_end->section_id,
                           original_handles[e.stream_id][section_end->name]});
  }
  // Creation/destruction skeletons preserve generation counters, including a
  // destroyed ID that will be reused inside the selected range. No old poses,
  // values, debug packets, frame sections, or simulation motion are replayed.
  for (std::size_t i = 0; i < begin; ++i)
    if (std::holds_alternative<CreateInstance>(source[i].payload) ||
        std::holds_alternative<DestroyInstance>(source[i].payload) ||
        std::holds_alternative<AddProfileZone>(source[i].payload) ||
        std::holds_alternative<AddProfileZoneEvent>(source[i].payload))
      crop.events.push_back(source[i]);
  std::map<std::uint64_t, std::map<std::string, std::uint32_t>>
      synthetic_handles;
  std::map<std::uint64_t, std::set<std::uint32_t>> occupied_handles;
  auto handle = [&](std::uint64_t stream, const std::string &text) {
    if (text.empty())
      return std::uint32_t(0);
    auto &context = synthetic_handles[stream];
    auto found = context.find(text);
    if (found != context.end())
      return found->second;
    auto &occupied = occupied_handles[stream];
    std::uint32_t h = 1;
    while (occupied.count(h)) {
      if (h == u32max)
        throw CropUnsupported("synthetic string handle table exhausted");
      ++h;
    }
    occupied.insert(h);
    context.emplace(text, h);
    emit(stream, StringHandleEvent{text, h});
    return h;
  };
  auto wire_name = [&](std::uint64_t stream, const QualifiedName &name) {
    auto ns = handle(stream, name.namespace_name),
         n = handle(stream, name.name);
    return WireName{ns, n};
  };
  for (const auto &instance : snapshot.instances) {
    const auto &state = instance.second;
    for (const auto &property : state.properties) {
      const auto &value = property.second;
      const auto *definition =
          session.metadata().find_property(state.class_name, property.first);
      if (definition && (definition->semantic == "profile event stream" ||
                         definition->semantic == "memory event stream"))
        continue;
      auto stream = bootstrap_stream;
      if (value.item_count > u32max)
        throw CropUnsupported("bootstrap property item count exceeds uint32; "
                              "complete full save remains available");
      if (value.incoming_type == QualifiedName{"physx3", "StringHandle"}) {
        if (!value.text)
          throw CropUnsupported(
              "bootstrap StringHandle property has no resolved text");
        auto h = value.as_uint();
        if (!h || *h > u32max)
          throw CropUnsupported("bootstrap StringHandle encoding invalid");
        stream = next_stream();
        const auto id = static_cast<std::uint32_t>(*h);
        occupied_handles[stream].insert(id);
        synthetic_handles[stream][*value.text] = id;
        emit(stream, StringHandleEvent{*value.text, id});
      }
      const auto prop = handle(stream, property.first);
      const auto type = wire_name(stream, value.incoming_type);
      const auto &bytes = value.raw_bytes();
      if (bytes.size() <= 1024u * 1024u)
        emit(stream, SetPropertyValue{
                         state.id, prop, type,
                         static_cast<std::uint32_t>(value.item_count), bytes});
      else {
        emit(stream, BeginSetPropertyValue{state.id, prop, type});
        for (std::size_t at = 0; at < bytes.size();) {
          const auto size =
              std::min<std::size_t>(1024u * 1024u, bytes.size() - at);
          Bytes part(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                     bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
          at += size;
          emit(stream, AppendPropertyValueData{
                           std::move(part),
                           at == bytes.size()
                               ? static_cast<std::uint32_t>(value.item_count)
                               : 0});
        }
        emit(stream, EndSetPropertyValue{});
      }
    }
    emit(bootstrap_stream, SetPickable{state.id, state.pickable});
    emit(bootstrap_stream, SetIsTopLevel{state.id, state.top_level});
    if (state.color)
      emit(bootstrap_stream, SetColor{state.id, *state.color});
  }
  auto existing_handle = [&](std::uint64_t stream, const std::string &text) {
    if (text.empty())
      return std::uint32_t(0);
    const auto table = original_handles.find(stream);
    if (table != original_handles.end())
      for (const auto &entry : table->second)
        if (entry.second == text)
          return entry.first;
    throw CropUnsupported("analysis bootstrap source stream lacks required "
                          "existing wire-name handle");
  };
  for (const auto &entry : provided) {
    const auto &channel = *entry.second;
    const auto property =
        existing_handle(channel.stream_id, channel.property_name);
    const WireName type{existing_handle(channel.stream_id, "physx3"),
                        existing_handle(channel.stream_id, "PvdU8")};
    emit(channel.stream_id,
         SetPropertyValue{channel.instance_id, property, type,
                          static_cast<std::uint32_t>(channel.bytes.size()),
                          channel.bytes});
  }
  for (const auto &camera : snapshot.cameras)
    emit(bootstrap_stream, SetCamera{camera.first, camera.second.position,
                                     camera.second.up, camera.second.target});
  for (const auto &shift : snapshot.origin_shifts)
    emit(bootstrap_stream, OriginShift{shift.first, shift.second});
  for (const auto &section : open_sections)
    crop.events.push_back(section.second);
  crop.bootstrap_event_count = crop.events.size();
  crop.events.insert(crop.events.end(),
                     source.begin() + static_cast<std::ptrdiff_t>(begin),
                     source.begin() + static_cast<std::ptrdiff_t>(end));
  Session validate(crop.header, crop.events);
  (void)validate;
  return crop;
}
CroppedStream crop_frames(const Session &session, std::size_t first,
                          std::size_t last, const CropOptions &options) {
  const auto &frames = session.frames();
  if (first > last || last >= frames.size())
    throw std::out_of_range("invalid crop native-frame range");
  if (!frames[first].begin_event_count)
    throw CropUnsupported("frame has no source BeginSection event");
  return crop_events(session, frames[first].begin_event_count - 1,
                     frames[last].event_count, options);
}
WriteResult save_crop(const std::string &path, const CroppedStream &crop,
                      const WriteOptions &options, bool overwrite) {
  // Validate semantic closure before publishing caller-modified crop events.
  Session validate(crop.header, crop.events);
  (void)validate;
  return save(path, overwrite, [&](auto &&sink) {
    return write(sink, crop.header, crop.events, crop.events.size(), options);
  });
}
} // namespace pvd::writing
