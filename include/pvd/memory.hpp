#pragma once
#include "pvd/analysis_export.hpp"
#include "pvd/session.hpp"
#include <iosfwd>
#include <limits>

namespace pvd::memory {
struct Source {
  std::uint64_t stream_id = 0, buffer_id = 0;
  bool operator<(const Source &rhs) const {
    return std::tie(stream_id, buffer_id) <
           std::tie(rhs.stream_id, rhs.buffer_id);
  }
  bool operator==(const Source &rhs) const {
    return stream_id == rhs.stream_id && buffer_id == rhs.buffer_id;
  }
};
enum class DiagnosticCode {
  malformed,
  unsupported_event,
  unsupported_full_allocation,
  limit_exceeded,
  undefined_string,
  conflicting_string,
  unknown_free,
  duplicate_allocation,
  counter_overflow
};
struct Diagnostic {
  DiagnosticCode code = DiagnosticCode::malformed;
  Source source;
  std::size_t event_index = 0, payload_offset = 0;
  std::string message;
};
enum class EndReason { none, freed, replaced_without_free };
struct Allocation {
  Source source;
  std::uint64_t address = 0, generation = 0, size = 0;
  std::uint32_t type_handle = 0, file_handle = 0, line = 0;
  std::optional<std::string> type_name, file;
  std::size_t allocated_event_count = 0, allocated_memory_index = 0;
  std::optional<std::size_t> ended_event_count, ended_memory_index;
  EndReason end_reason = EndReason::none;
};
enum class EventKind { string_table, allocation, deallocation };
struct Event {
  Source source;
  std::size_t event_index = 0, payload_offset = 0;
  EventKind kind = EventKind::string_table;
  std::uint32_t string_handle = 0;
  std::string string_value;
  std::uint64_t address = 0;
  std::optional<std::size_t> allocation_index, replaced_allocation_index;
};
struct Stats {
  std::uint64_t current_bytes = 0, peak_bytes = 0, allocated_bytes = 0,
                freed_bytes = 0, replaced_bytes = 0;
  std::size_t current_allocations = 0, peak_allocations = 0,
              allocation_count = 0, free_count = 0, unknown_free_count = 0;
};
enum class GroupBy { type, file, file_line, type_file_line };
struct GroupStats {
  std::optional<std::string> type_name, file;
  std::uint32_t line = 0;
  Stats stats;
};
struct Snapshot {
  std::size_t event_count =
      0; // Core outer-event prefix; never a fabricated allocation timestamp.
  Stats totals;
  std::vector<Allocation> active;
  std::vector<GroupStats> groups;
  std::vector<Diagnostic> diagnostics;
  bool complete = true;
};
struct Options {
  std::size_t max_packet_bytes = 64u * 1024u * 1024u;
  std::size_t max_string_bytes = 1024u * 1024u;
  std::size_t max_strings_per_source = 1024u * 1024u;
  std::size_t max_memory_events = 4u * 1024u * 1024u;
  std::size_t max_allocations = 2u * 1024u * 1024u;
};
struct Filter {
  std::optional<Source> source;
  std::optional<std::uint64_t> address;
  std::optional<std::string> type_name, file;
  std::optional<std::uint32_t> line;
  std::string text;
  bool active_only = false;
};
// Parses the SDK 3.4 'memory event stream' channel, preserving source
// isolation. FullAllocationEvent is an in-process recorder object, not a
// serialized 3.4 packet. Wire tag 4 is diagnosed, not guessed or
// length-skipped.
class Index {
public:
  PVD_ANALYSIS_API explicit Index(Options options = {});
  PVD_ANALYSIS_API explicit Index(const Session &, Options options = {});
  PVD_ANALYSIS_API explicit Index(const std::vector<ChannelPacket> &,
                                  Options options = {});
  PVD_ANALYSIS_API ~Index();
  PVD_ANALYSIS_API Index(Index &&) noexcept;
  PVD_ANALYSIS_API Index &operator=(Index &&) noexcept;
  Index(const Index &) = delete;
  Index &operator=(const Index &) = delete;
  // Bind to one Session; only new channel packets before this prefix are read.
  // Earlier query prefixes do not rewind parser/string-table state.
  PVD_ANALYSIS_API void
  update(const Session &,
         std::size_t event_count = std::numeric_limits<std::size_t>::max());
  PVD_ANALYSIS_API const std::vector<Event> &events() const noexcept;
  PVD_ANALYSIS_API const std::vector<Allocation> &allocations() const noexcept;
  PVD_ANALYSIS_API const std::vector<Diagnostic> &diagnostics() const noexcept;
  PVD_ANALYSIS_API Snapshot snapshot_at_event(
      std::size_t event_count, GroupBy by = GroupBy::type_file_line) const;
  // Same totals/groups/diagnostics as snapshot_at_event, without copying live
  // allocation rows. Useful for a frame-dependent summary with paged history.
  PVD_ANALYSIS_API Snapshot summary_at_event(
      std::size_t event_count, GroupBy by = GroupBy::type_file_line) const;
  PVD_ANALYSIS_API std::vector<Allocation>
  history_at_event(std::size_t event_count, Filter filter = {},
                   std::size_t offset = 0, std::size_t limit = 1000) const;
  PVD_ANALYSIS_API std::size_t matching_count(std::size_t event_count,
                                              Filter filter = {}) const;
  PVD_ANALYSIS_API void write_csv(std::ostream &, std::size_t event_count,
                                  Filter filter = {}) const;
  // All observed allocations at this prefix, grouped by the requested key.
  // History text/active filters do not alter this explicitly full-scope export.
  PVD_ANALYSIS_API void
  write_groups_csv(std::ostream &, std::size_t event_count,
                   GroupBy by = GroupBy::type_file_line) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::memory
