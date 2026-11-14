#include "pvd/profile.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <set>
#include <sstream>

namespace pvd::profile {
namespace {
struct Issue {
  DiagnosticCode code;
  std::size_t offset;
  const char *message;
};
struct Reader {
  const Bytes &bytes;
  ByteOrder order;
  std::size_t offset = 0;
  std::uint64_t integer(std::size_t width) {
    if (width > bytes.size() - offset)
      throw Issue{DiagnosticCode::truncated_packet, offset,
                  "profile event ends inside a field"};
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
      const auto shift = order == ByteOrder::little ? i : width - 1 - i;
      value |= std::uint64_t(bytes[offset + i]) << (shift * 8);
    }
    offset += width;
    return value;
  }
  void skip(std::size_t size) {
    if (size > bytes.size() - offset)
      throw Issue{DiagnosticCode::truncated_packet, offset,
                  "GPU profile payload exceeds packet"};
    offset += size;
  }
};
std::size_t width(unsigned flags) { return std::size_t(1) << (flags & 3); }
std::int64_t signed64(std::uint64_t value) {
  if (value <= std::uint64_t(std::numeric_limits<std::int64_t>::max()))
    return static_cast<std::int64_t>(value);
  return -static_cast<std::int64_t>(~value) - 1;
}
std::int16_t legacy16(std::uint64_t value) {
  const auto low = static_cast<std::uint16_t>(value);
  return low <= 32767
             ? static_cast<std::int16_t>(low)
             : static_cast<std::int16_t>(-1 - static_cast<int>(65535 - low));
}
std::string csv(const std::string &value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '\"')
      out += '\"';
    out += c;
  }
  return out + '\"';
}
using ScopeKey =
    std::tuple<ZoneKey, std::uint32_t, std::uint64_t, std::uint16_t>;
using ThreadKey = std::tuple<ZoneKey, std::uint32_t, std::uint64_t>;
} // namespace

bool is_profile_packet(const ChannelPacket &packet) noexcept {
  return packet.semantic == "profile event stream";
}

struct Index::Impl {
  StreamHeader header;
  Options options;
  Statistics stats;
  std::map<ZoneKey, Zone> zones;
  std::vector<Event> events;
  std::vector<Interval> intervals;
  std::vector<Diagnostic> diagnostics;
  std::map<ScopeKey, std::vector<std::size_t>> open;
  struct Depths {
    std::size_t next = 0, active = 0;
    std::set<std::size_t> available;
  };
  std::map<ThreadKey, Depths> depths;
  std::map<std::size_t, std::size_t> start_depths;
  struct Life {
    std::uint64_t generation = 0;
    bool created = false, alive = false, prepared = false;
  };
  std::map<std::uint64_t, Life> life;
  std::set<std::tuple<ZoneKey, DiagnosticCode, std::uint16_t>> warned;
  std::size_t names = 0, channel_cursor = 0;
  const Session *bound_session = nullptr;
  explicit Impl(StreamHeader value, Options bounds)
      : header(value), options(bounds) {
    if (!options.max_packet_bytes || !options.max_events ||
        !options.max_intervals || !options.max_open_scopes ||
        !options.max_diagnostics || !options.max_zones ||
        !options.max_event_names || !options.max_name_bytes)
      throw std::invalid_argument("profile analysis limits must be positive");
  }
  void diagnostic(Severity severity, DiagnosticCode code, Source source,
                  std::string message) {
    if (severity != Severity::info)
      stats.complete = false;
    if (diagnostics.size() < options.max_diagnostics)
      diagnostics.push_back({severity, code, source, std::move(message)});
    else
      ++stats.suppressed_diagnostics;
  }
  void once(Severity severity, DiagnosticCode code, Source source,
            std::uint16_t event, const char *message) {
    const auto key = std::make_tuple(source.zone, code, event);
    if (warned.count(key))
      return;
    if (warned.size() < options.max_diagnostics)
      warned.insert(key);
    diagnostic(severity, code, source, message);
  }
  void abandon(ZoneKey zone, const Source &where, const char *reason) {
    for (auto it = open.begin(); it != open.end();) {
      if (!(std::get<0>(it->first) == zone)) {
        ++it;
        continue;
      }
      for (auto index : it->second) {
        diagnostic(Severity::warning, DiagnosticCode::abandoned_start,
                   events[index].source, reason);
        start_depths.erase(index);
        --stats.open_scopes;
      }
      it = open.erase(it);
    }
    for (auto it = depths.begin(); it != depths.end();)
      if (std::get<0>(it->first) == zone)
        it = depths.erase(it);
      else
        ++it;
    (void)where;
  }
  void limit(Source source, const char *message) {
    stats.saturated = true;
    diagnostic(Severity::error, DiagnosticCode::resource_limit, source,
               message);
    open.clear();
    depths.clear();
    start_depths.clear();
    stats.open_scopes = 0;
  }
  void zone(ZoneKey key, std::string name) {
    if (stats.saturated)
      return;
    if (name.size() > options.max_name_bytes) {
      limit({0, 0, 0, 0, key}, "profile zone name exceeds configured limit");
      return;
    }
    auto found = zones.find(key);
    if (found == zones.end()) {
      if (zones.size() >= options.max_zones) {
        limit({0, 0, 0, 0, key}, "profile zone count exceeds configured limit");
        return;
      }
      zones.emplace(key, Zone{key, std::move(name), {}});
    } else
      found->second.name = std::move(name);
    life[key.id].generation = key.generation;
  }
  void event_name(ZoneKey key, std::uint16_t id, std::string name,
                  bool enabled) {
    if (stats.saturated)
      return;
    if (!zones.count(key))
      zone(key, "zone " + std::to_string(key.id));
    if (stats.saturated)
      return;
    if (name.size() > options.max_name_bytes) {
      limit({0, 0, 0, 0, key}, "profile event name exceeds configured limit");
      return;
    }
    auto &values = zones.at(key).event_names;
    if (!values.count(id)) {
      if (names >= options.max_event_names) {
        limit({0, 0, 0, 0, key},
              "profile event name count exceeds configured limit");
        return;
      }
      ++names;
    }
    values[id] = {std::move(name), enabled};
  }
  std::optional<double> nanos(long double ticks) const noexcept {
    if (!header.timestamp_numerator || !header.timestamp_denominator)
      return {};
    const auto value = ticks *
                       static_cast<long double>(header.timestamp_numerator) /
                       static_cast<long double>(header.timestamp_denominator);
    const auto converted = static_cast<double>(value);
    return std::isfinite(converted) ? std::optional<double>(converted)
                                    : std::nullopt;
  }
  void commit(Event event) {
    if (events.size() >= options.max_events) {
      limit(event.source,
            "profile decoded event count exceeds configured limit");
      return;
    }
    if (event.kind == Kind::start &&
        stats.open_scopes >= options.max_open_scopes) {
      limit(event.source, "profile open scope count exceeds configured limit");
      return;
    }
    const auto event_index = events.size();
    events.push_back(std::move(event));
    stats.decoded_events = events.size();
    const auto &value = events.back();
    if (value.kind == Kind::gpu)
      return;
    auto zone = zones.find(value.source.zone);
    if (zone == zones.end())
      once(Severity::warning, DiagnosticCode::missing_zone, value.source, 0,
           "profile packet has no advertised zone metadata");
    if (zone == zones.end() || !zone->second.event_names.count(value.event_id))
      once(Severity::warning, DiagnosticCode::missing_event_name, value.source,
           value.event_id, "profile event ID has no advertised name");
    if (value.kind == Kind::value) {
      once(Severity::info, DiagnosticCode::value_without_timestamp,
           value.source, value.event_id,
           "profile counter value has no timestamp; excluded from CPU duration "
           "pairing");
      if (*value.value != *value.sdk_legacy_value)
        once(Severity::warning, DiagnosticCode::legacy_value_truncation,
             value.source, value.event_id,
             "SDK 3.4 getValue truncates to int16; raw signed 64-bit value is "
             "preserved separately");
      return;
    }
    const ScopeKey key{value.source.zone, value.thread_id, value.context_id,
                       value.event_id};
    const ThreadKey thread{value.source.zone, value.thread_id,
                           value.context_id};
    if (value.kind == Kind::start) {
      auto &lanes = depths[thread];
      std::size_t depth = 0;
      if (lanes.available.empty())
        depth = lanes.next++;
      else {
        depth = *lanes.available.begin();
        lanes.available.erase(lanes.available.begin());
      }
      ++lanes.active;
      start_depths[event_index] = depth;
      open[key].push_back(event_index);
      ++stats.open_scopes;
      return;
    }
    auto found = open.find(key);
    if (found == open.end() || found->second.empty()) {
      diagnostic(Severity::warning, DiagnosticCode::unmatched_stop,
                 value.source,
                 "profile stop has no matching start in this "
                 "zone/thread/context/event lane");
      return;
    }
    const auto start_index = found->second.back();
    const auto &start = events[start_index];
    const auto depth = start_depths.at(start_index);
    found->second.pop_back();
    if (found->second.empty())
      open.erase(found);
    start_depths.erase(start_index);
    --stats.open_scopes;
    auto thread_depth = depths.find(thread);
    if (thread_depth != depths.end()) {
      if (--thread_depth->second.active == 0)
        depths.erase(thread_depth);
      else
        thread_depth->second.available.insert(depth);
    }
    if (*value.timestamp_ticks < *start.timestamp_ticks) {
      diagnostic(Severity::warning, DiagnosticCode::backwards_interval,
                 value.source,
                 "profile stop timestamp precedes its matching start; no "
                 "negative or zero duration is fabricated");
      return;
    }
    if (intervals.size() >= options.max_intervals) {
      limit(value.source, "profile interval count exceeds configured limit");
      return;
    }
    const auto duration = *value.timestamp_ticks - *start.timestamp_ticks;
    intervals.push_back({start.source, value.source, value.event_id,
                         start.thread_id, start.context_id, start.cpu_id,
                         start.thread_priority, *start.timestamp_ticks,
                         *value.timestamp_ticks, duration,
                         nanos(static_cast<long double>(duration)), depth});
    stats.paired_intervals = intervals.size();
  }
  void packet(const ChannelPacket &packet) {
    if (!is_profile_packet(packet))
      return;
    if (stats.finished)
      throw std::logic_error("cannot append to a finished profile index");
    const auto packet_index = stats.profile_packets++;
    if (stats.saturated)
      return;
    const auto generation = life.find(packet.instance_id);
    const ZoneKey key{packet.instance_id, generation == life.end()
                                              ? 0
                                              : generation->second.generation};
    Source source{packet.event_index, packet_index, 0, packet.stream_id, key};
    if (!header.timestamp_numerator || !header.timestamp_denominator)
      once(Severity::warning, DiagnosticCode::invalid_clock, source, 0,
           "counter calibration unavailable; durations remain ticks, never "
           "assumed nanoseconds");
    const auto &bytes = packet.value.raw_bytes();
    if (bytes.size() > options.max_packet_bytes) {
      limit(source, "profile packet exceeds configured byte limit");
      return;
    }
    if (packet.value.incoming_type != QualifiedName{"physx3", "PvdU8"} ||
        packet.value.item_count != bytes.size() || packet.value.opaque) {
      ++stats.malformed_packets;
      diagnostic(Severity::error, DiagnosticCode::malformed_packet, source,
                 "profile channel must contain a complete PvdU8 array");
      abandon(key, source,
              "open profile start abandoned after malformed channel value");
      return;
    }
    Reader reader{bytes, packet.value.byte_order};
    bool has_context = false;
    std::uint32_t thread = 0;
    std::uint64_t context = 0, last_timestamp = 0;
    std::uint8_t priority = 0, cpu = 0;
    try {
      while (reader.offset < bytes.size() && !stats.saturated) {
        source.payload_offset = reader.offset;
        const auto tag = static_cast<unsigned>(reader.integer(1));
        const auto flags = static_cast<unsigned>(reader.integer(1));
        const auto id = static_cast<std::uint16_t>(reader.integer(2));
        if (tag < 1 || tag > 6)
          throw Issue{
              DiagnosticCode::unknown_event, source.payload_offset,
              "unknown profile tag has no length and cannot be skipped"};
        Event value;
        value.source = source;
        value.event_id = id;
        if (tag <= 4) {
          if ((flags & 0xf0) || (tag >= 3 && (flags & 0xfc)))
            throw Issue{DiagnosticCode::malformed_packet, source.payload_offset,
                        "profile start/stop uses reserved compression bits"};
          if (tag <= 2) {
            thread = static_cast<std::uint32_t>(reader.integer(4));
            context = reader.integer(width(flags >> 2));
            priority = static_cast<std::uint8_t>(reader.integer(1));
            cpu = static_cast<std::uint8_t>(reader.integer(1));
            has_context = true;
          } else if (!has_context)
            throw Issue{DiagnosticCode::relative_without_context,
                        source.payload_offset,
                        "relative profile event appears before any full "
                        "context in this flushed packet"};
          const auto timestamp = reader.integer(width(flags));
          if ((flags & 3) != 3 &&
              timestamp >
                  std::numeric_limits<std::uint64_t>::max() - last_timestamp)
            throw Issue{DiagnosticCode::timestamp_overflow,
                        source.payload_offset,
                        "compressed profile timestamp overflows uint64"};
          last_timestamp =
              (flags & 3) == 3 ? timestamp : last_timestamp + timestamp;
          value.kind = (tag == 1 || tag == 3) ? Kind::start : Kind::stop;
          value.thread_id = thread;
          value.context_id = context;
          value.cpu_id = cpu;
          value.thread_priority = priority;
          value.timestamp_ticks = last_timestamp;
          value.relative_context = tag >= 3;
          value.relative_timestamp = (flags & 3) != 3;
        } else if (tag == 5) {
          if (flags & 0xf0)
            throw Issue{DiagnosticCode::malformed_packet, source.payload_offset,
                        "profile value uses reserved compression bits"};
          const auto raw = reader.integer(width(flags));
          value.context_id = reader.integer(width(flags >> 2));
          value.thread_id = static_cast<std::uint32_t>(reader.integer(4));
          value.kind = Kind::value;
          value.value = signed64(raw);
          value.sdk_legacy_value = legacy16(raw);
        } else {
          value.kind = Kind::gpu;
          (void)reader.integer(8);
          (void)reader.integer(4);
          const auto count = reader.integer(4);
          reader.skip(static_cast<std::size_t>(count));
          const auto repeated = reader.integer(4);
          value.gpu_version = static_cast<std::uint32_t>(reader.integer(4));
          if (count != repeated)
            throw Issue{DiagnosticCode::malformed_packet, source.payload_offset,
                        "obsolete CUDA payload lengths disagree"};
          value.gpu_payload_bytes = static_cast<std::uint32_t>(count);
          ++stats.skipped_gpu_events;
          diagnostic(Severity::warning, DiagnosticCode::unsupported_gpu, source,
                     "obsolete CUDA profile payload retained as metadata only; "
                     "GPU timing is not decoded or presented as CPU time");
        }
        commit(std::move(value));
      }
    } catch (const Issue &issue) {
      ++stats.malformed_packets;
      source.payload_offset = issue.offset;
      diagnostic(Severity::error, issue.code, source, issue.message);
      abandon(key, source,
              "open profile start abandoned after corrupt/truncated packet; "
              "later stops will not pair across missing data");
    }
  }
  void outer(const pvd::Event &event, std::size_t index) {
    if (stats.saturated)
      return;
    const Source source{index, stats.profile_packets, 0, event.stream_id, {}};
    if (const auto *announcement =
            std::get_if<AddProfileZone>(&event.payload)) {
      auto &state = life[announcement->instance_id];
      if (state.created && !state.alive && !state.prepared) {
        ++state.generation;
        state.prepared = true;
      }
      zone({announcement->instance_id, state.generation}, announcement->name);
    } else if (const auto *event_name_record =
                   std::get_if<AddProfileZoneEvent>(&event.payload)) {
      event_name({event_name_record->instance_id,
                  life[event_name_record->instance_id].generation},
                 event_name_record->event_id, event_name_record->name,
                 event_name_record->compile_time_enabled);
    } else if (const auto *creation =
                   std::get_if<CreateInstance>(&event.payload)) {
      auto found = life.find(creation->instance_id);
      if (found != life.end()) {
        auto &state = found->second;
        if (state.created && !state.alive && !state.prepared)
          ++state.generation;
        state.created = state.alive = true;
        state.prepared = false;
      }
    } else if (const auto *destruction =
                   std::get_if<DestroyInstance>(&event.payload)) {
      auto found = life.find(destruction->instance_id);
      if (found != life.end()) {
        auto where = source;
        where.zone = {destruction->instance_id, found->second.generation};
        abandon(where.zone, where,
                "profile zone destroyed with an outstanding start");
        found->second.alive = false;
      }
    }
  }
};

Index::Index(StreamHeader header, Options options)
    : impl_(std::make_unique<Impl>(header, options)) {}
Index::Index(const Session &session, Options options)
    : Index(session.header(), options) {
  update(session);
  if (session.finished() || session.failed())
    finish();
}
Index::~Index() = default;
Index::Index(Index &&) noexcept = default;
Index &Index::operator=(Index &&) noexcept = default;
void Index::register_zone(ZoneKey key, std::string name) {
  if (impl_->stats.finished)
    throw std::logic_error("profile index is finished");
  impl_->zone(key, std::move(name));
}
void Index::register_event(ZoneKey key, std::uint16_t id, std::string name,
                           bool enabled) {
  if (impl_->stats.finished)
    throw std::logic_error("profile index is finished");
  impl_->event_name(key, id, std::move(name), enabled);
}
void Index::append_packet(const ChannelPacket &packet) {
  if (!is_profile_packet(packet))
    return;
  impl_->packet(packet);
  impl_->stats.indexed_event_count =
      std::max(impl_->stats.indexed_event_count,
               packet.event_index == std::numeric_limits<std::size_t>::max()
                   ? packet.event_index
                   : packet.event_index + 1);
}
void Index::update(const Session &session, std::size_t requested) {
  if (impl_->stats.finished)
    throw std::logic_error("profile index is finished");
  if (impl_->bound_session && impl_->bound_session != &session)
    throw std::invalid_argument("profile index cannot append another Session");
  if (!impl_->bound_session && impl_->stats.indexed_event_count)
    throw std::logic_error(
        "cannot mix manual profile packets with Session indexing");
  const auto end = requested == std::numeric_limits<std::size_t>::max()
                       ? session.event_count()
                       : requested;
  if (end > session.event_count() || end < impl_->stats.indexed_event_count)
    throw std::out_of_range("profile Session prefix is not an append");
  if (!impl_->bound_session)
    impl_->header = session.header();
  impl_->bound_session = &session;
  const auto &channels = session.channels();
  for (auto index = impl_->stats.indexed_event_count; index < end; ++index) {
    impl_->outer(session.events()[index], index);
    while (impl_->channel_cursor < channels.size() &&
           channels[impl_->channel_cursor].event_index <= index) {
      const auto &packet = channels[impl_->channel_cursor++];
      if (packet.event_index == index)
        impl_->packet(packet);
    }
    impl_->stats.indexed_event_count = index + 1;
  }
}
void Index::finish() {
  if (impl_->stats.finished)
    return;
  for (const auto &lane : impl_->open)
    for (auto index : lane.second)
      impl_->diagnostic(Severity::warning, DiagnosticCode::unclosed_start,
                        impl_->events[index].source,
                        "capture ends before this profile scope stops; no "
                        "fabricated duration is emitted");
  impl_->stats.finished = true;
}
const std::map<ZoneKey, Zone> &Index::zones() const noexcept {
  return impl_->zones;
}
const std::vector<Event> &Index::events() const noexcept {
  return impl_->events;
}
const std::vector<Interval> &Index::intervals() const noexcept {
  return impl_->intervals;
}
const std::vector<Diagnostic> &Index::diagnostics() const noexcept {
  return impl_->diagnostics;
}
Statistics Index::statistics() const noexcept { return impl_->stats; }
std::string Index::zone_name(ZoneKey key) const {
  auto found = impl_->zones.find(key);
  return found == impl_->zones.end() ? "zone " + std::to_string(key.id)
                                     : found->second.name;
}
std::string Index::event_name(ZoneKey key, std::uint16_t event) const {
  auto zone = impl_->zones.find(key);
  if (zone != impl_->zones.end()) {
    auto name = zone->second.event_names.find(event);
    if (name != zone->second.event_names.end())
      return name->second.name;
  }
  return "#" + std::to_string(event);
}
std::optional<double> Index::nanoseconds(std::uint64_t ticks) const noexcept {
  return impl_->nanos(static_cast<long double>(ticks));
}
std::vector<std::size_t> Index::query(const Query &filter) const {
  if (filter.begin_event_count > filter.end_event_count ||
      (filter.begin_ticks && filter.end_ticks &&
       *filter.begin_ticks > *filter.end_ticks))
    throw std::invalid_argument("inverted profile query range");
  std::vector<std::size_t> result;
  if (filter.begin_ticks && filter.end_ticks &&
      *filter.begin_ticks == *filter.end_ticks)
    return result;
  for (std::size_t i = 0; i < impl_->intervals.size(); ++i) {
    const auto &span = impl_->intervals[i];
    const auto zone = span.start_source.zone;
    if ((filter.zone_id && *filter.zone_id != zone.id) ||
        (filter.zone_generation &&
         *filter.zone_generation != zone.generation) ||
        (filter.thread_id && *filter.thread_id != span.thread_id) ||
        (filter.context_id && *filter.context_id != span.context_id) ||
        (filter.event_id && *filter.event_id != span.event_id) ||
        span.stop_source.event_index >= filter.end_event_count ||
        span.stop_source.event_index < filter.begin_event_count ||
        (filter.begin_ticks && span.end_ticks <= *filter.begin_ticks) ||
        (filter.end_ticks && span.begin_ticks >= *filter.end_ticks))
      continue;
    if (!filter.text.empty() &&
        zone_name(zone).find(filter.text) == std::string::npos &&
        event_name(zone, span.event_id).find(filter.text) == std::string::npos)
      continue;
    result.push_back(i);
  }
  return result;
}
std::vector<Aggregate> Index::aggregate(const Query &filter,
                                        bool by_thread) const {
  std::map<std::tuple<ZoneKey, std::uint16_t, std::optional<std::uint32_t>>,
           Aggregate>
      rows;
  for (auto index : query(filter)) {
    const auto &span = impl_->intervals[index];
    const auto thread =
        by_thread ? std::optional<std::uint32_t>(span.thread_id) : std::nullopt;
    auto &row = rows[{span.start_source.zone, span.event_id, thread}];
    if (!row.count) {
      row.zone = span.start_source.zone;
      row.event_id = span.event_id;
      row.thread_id = thread;
      row.zone_name = zone_name(row.zone);
      row.event_name = event_name(row.zone, row.event_id);
      row.minimum_ticks = span.duration_ticks;
    }
    ++row.count;
    row.total_ticks += static_cast<long double>(span.duration_ticks);
    row.minimum_ticks = std::min(row.minimum_ticks, span.duration_ticks);
    row.maximum_ticks = std::max(row.maximum_ticks, span.duration_ticks);
  }
  std::vector<Aggregate> result;
  for (auto &entry : rows) {
    auto &row = entry.second;
    row.total_nanoseconds = impl_->nanos(row.total_ticks);
    row.mean_nanoseconds =
        impl_->nanos(row.total_ticks / static_cast<long double>(row.count));
    row.minimum_nanoseconds =
        impl_->nanos(static_cast<long double>(row.minimum_ticks));
    row.maximum_nanoseconds =
        impl_->nanos(static_cast<long double>(row.maximum_ticks));
    result.push_back(std::move(row));
  }
  return result;
}
void Index::write_csv(std::ostream &out, const Query &filter) const {
  out << "zone_id,zone_generation,zone,event_id,event,thread_id,context_id,"
         "start_event,stop_event,start_ticks,end_ticks,duration_ticks,duration_"
         "ns,cpu,priority,depth\n";
  for (auto index : query(filter)) {
    const auto &span = impl_->intervals[index];
    const auto zone = span.start_source.zone;
    out << zone.id << ',' << zone.generation << ',' << csv(zone_name(zone))
        << ',' << span.event_id << ',' << csv(event_name(zone, span.event_id))
        << ',' << span.thread_id << ',' << span.context_id << ','
        << span.start_source.event_index << ',' << span.stop_source.event_index
        << ',' << span.begin_ticks << ',' << span.end_ticks << ','
        << span.duration_ticks << ',';
    if (span.duration_nanoseconds)
      out << std::setprecision(17) << *span.duration_nanoseconds;
    out << ',' << unsigned(span.cpu_id) << ',' << unsigned(span.thread_priority)
        << ',' << span.depth << '\n';
  }
  if (!out)
    throw std::runtime_error("profile CSV output failed");
}
} // namespace pvd::profile
