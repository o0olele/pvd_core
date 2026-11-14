#pragma once
#include "pvd/analysis_export.hpp"
#include "pvd/session.hpp"
#include <iosfwd>
#include <limits>

namespace pvd::profile {
struct ZoneKey {
  std::uint64_t id = 0, generation = 0;
  bool operator<(const ZoneKey &o) const {
    return std::tie(id, generation) < std::tie(o.id, o.generation);
  }
  bool operator==(const ZoneKey &o) const {
    return id == o.id && generation == o.generation;
  }
};
struct EventName {
  std::string name;
  bool compile_time_enabled = false;
};
struct Zone {
  ZoneKey key;
  std::string name;
  std::map<std::uint16_t, EventName> event_names;
};
struct Source {
  std::size_t event_index = 0, packet_index = 0, payload_offset = 0;
  std::uint64_t stream_id = 0;
  ZoneKey zone;
};
enum class Kind { start, stop, value, gpu };
struct Event {
  Kind kind = Kind::start;
  Source source;
  std::uint16_t event_id = 0;
  std::uint32_t thread_id = 0;
  std::uint64_t context_id = 0;
  std::uint8_t cpu_id = 0, thread_priority = 0;
  // Native counter ticks, not the stale SDK field's "tens of ns" spelling.
  std::optional<std::uint64_t> timestamp_ticks;
  std::optional<std::int64_t> value;
  // SDK 3.4 EventValue::getValue() truncates to int16_t. Preserve both views.
  std::optional<std::int16_t> sdk_legacy_value;
  bool relative_context = false, relative_timestamp = false;
  std::uint32_t gpu_version = 0, gpu_payload_bytes = 0;
};
struct Interval {
  Source start_source, stop_source;
  std::uint16_t event_id = 0;
  std::uint32_t thread_id = 0;
  std::uint64_t context_id = 0;
  std::uint8_t cpu_id = 0, thread_priority = 0;
  std::uint64_t begin_ticks = 0, end_ticks = 0, duration_ticks = 0;
  std::optional<double> duration_nanoseconds;
  // Non-overlapping display lane within this zone/thread/context. Properly
  // nested scopes occupy increasing depths; crossing event IDs remain valid.
  std::size_t depth = 0;
};
enum class Severity { info, warning, error };
enum class DiagnosticCode {
  invalid_clock,
  malformed_packet,
  truncated_packet,
  unknown_event,
  relative_without_context,
  timestamp_overflow,
  backwards_interval,
  missing_zone,
  missing_event_name,
  unmatched_stop,
  unclosed_start,
  abandoned_start,
  unsupported_gpu,
  value_without_timestamp,
  legacy_value_truncation,
  resource_limit
};
struct Diagnostic {
  Severity severity = Severity::warning;
  DiagnosticCode code = DiagnosticCode::malformed_packet;
  Source source;
  std::string message;
};
struct Options {
  std::size_t max_packet_bytes = 64u * 1024u * 1024u;
  std::size_t max_events = 2u * 1024u * 1024u, max_intervals = 1024u * 1024u;
  std::size_t max_open_scopes = 128u * 1024u, max_diagnostics = 4096;
  std::size_t max_zones = 4096, max_event_names = 1024u * 1024u,
              max_name_bytes = 4096;
};
struct Statistics {
  std::size_t profile_packets = 0, decoded_events = 0, paired_intervals = 0,
              open_scopes = 0;
  std::size_t skipped_gpu_events = 0, malformed_packets = 0,
              suppressed_diagnostics = 0;
  std::size_t indexed_event_count = 0;
  bool complete = true, finished = false, saturated = false;
};
struct Query {
  // Case-sensitive substring of the advertised zone or event name.
  std::string text;
  std::optional<std::uint64_t> zone_id, zone_generation, context_id;
  std::optional<std::uint32_t> thread_id;
  std::optional<std::uint16_t> event_id;
  // Only intervals completed before end_event_count are returned. A start
  // may precede begin_event_count if its matching stop is inside the range.
  std::size_t begin_event_count = 0,
              end_event_count = std::numeric_limits<std::size_t>::max();
  // Counter-tick overlap filter [begin_ticks,end_ticks), independent of
  // source-event range.
  std::optional<std::uint64_t> begin_ticks, end_ticks;
};
struct Aggregate {
  ZoneKey zone;
  std::uint16_t event_id = 0;
  std::optional<std::uint32_t> thread_id;
  std::string zone_name, event_name;
  std::size_t count = 0;
  long double total_ticks = 0;
  std::uint64_t minimum_ticks = 0, maximum_ticks = 0;
  std::optional<double> total_nanoseconds, mean_nanoseconds,
      minimum_nanoseconds, maximum_nanoseconds;
};

// Standalone analysis; no SDK or UI dependency. Not internally synchronized.
// Each SDK flush packet resets compression context/timestamp state. Open CPU
// scopes intentionally survive packets and pair LIFO by zone/thread/context/id.
class Index {
public:
  explicit PVD_ANALYSIS_API Index(StreamHeader header = {},
                                  Options options = {});
  explicit PVD_ANALYSIS_API Index(const Session &, Options options = {});
  PVD_ANALYSIS_API ~Index();
  PVD_ANALYSIS_API Index(Index &&) noexcept;
  PVD_ANALYSIS_API Index &operator=(Index &&) noexcept;
  Index(const Index &) = delete;
  Index &operator=(const Index &) = delete;

  // Append the next immutable Session prefix. Rewinding/replacing the bound
  // session is rejected; construct another Index for another capture.
  PVD_ANALYSIS_API void
  update(const Session &,
         std::size_t event_count = std::numeric_limits<std::size_t>::max());
  PVD_ANALYSIS_API void register_zone(ZoneKey, std::string name);
  PVD_ANALYSIS_API void register_event(ZoneKey, std::uint16_t, std::string name,
                                       bool compile_time_enabled = true);
  // Low-level use for tests or an already selected channel. Other semantics
  // are ignored; malformed profile packets produce explicit diagnostics.
  PVD_ANALYSIS_API void append_packet(const ChannelPacket &);
  PVD_ANALYSIS_API void finish();

  PVD_ANALYSIS_API const std::map<ZoneKey, Zone> &zones() const noexcept;
  PVD_ANALYSIS_API const std::vector<Event> &events() const noexcept;
  PVD_ANALYSIS_API const std::vector<Interval> &intervals() const noexcept;
  PVD_ANALYSIS_API const std::vector<Diagnostic> &diagnostics() const noexcept;
  PVD_ANALYSIS_API Statistics statistics() const noexcept;
  PVD_ANALYSIS_API std::string zone_name(ZoneKey) const;
  PVD_ANALYSIS_API std::string event_name(ZoneKey, std::uint16_t) const;
  PVD_ANALYSIS_API std::optional<double>
  nanoseconds(std::uint64_t ticks) const noexcept;
  PVD_ANALYSIS_API std::vector<std::size_t>
  query(const Query &filter = {}) const;
  PVD_ANALYSIS_API std::vector<Aggregate>
  aggregate(const Query &filter = {}, bool group_by_thread = false) const;
  // RFC4180-escaped names; inclusive durations (nested scopes are not
  // subtracted).
  PVD_ANALYSIS_API void write_csv(std::ostream &,
                                  const Query &filter = {}) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
PVD_ANALYSIS_API bool is_profile_packet(const ChannelPacket &) noexcept;
} // namespace pvd::profile
