#include "pvd/memory.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <ostream>
#include <set>
#include <sstream>

namespace pvd::memory {
namespace {
struct FormatGuard {
  std::ostream &out;
  std::ios_base::fmtflags flags;
  explicit FormatGuard(std::ostream &stream)
      : out(stream), flags(stream.flags()) {
    out << std::dec << std::noboolalpha;
  }
  ~FormatGuard() { out.flags(flags); }
};
struct Problem {
  DiagnosticCode code;
  std::string message;
  std::size_t offset;
};
class Reader {
public:
  Reader(const Bytes &bytes, ByteOrder order) : bytes_(bytes), order_(order) {}
  std::size_t position() const { return position_; }
  std::size_t remaining() const { return bytes_.size() - position_; }
  std::uint64_t integer(std::size_t width) {
    if (width > remaining())
      throw Problem{DiagnosticCode::malformed, "truncated memory event field",
                    position_};
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
      auto j = order_ == ByteOrder::little ? i : width - i - 1;
      value |= std::uint64_t(bytes_[position_ + j]) << (8 * i);
    }
    position_ += width;
    return value;
  }
  std::uint64_t compressed(std::uint16_t header, unsigned shift,
                           bool address = false) {
    const auto code = (header >> shift) & 3;
    const std::size_t width = code == 0   ? 1
                              : code == 1 ? 2
                              : code == 2 ? 4
                              : address   ? 8
                                          : 4;
    return integer(width);
  }
  std::string string(std::size_t limit) {
    auto at = position_;
    const auto n = integer(4);
    if (n > limit)
      throw Problem{DiagnosticCode::limit_exceeded,
                    "memory string exceeds byte limit", at};
    if (!n || n > remaining())
      throw Problem{DiagnosticCode::malformed,
                    "memory string length exceeds packet or omits terminator",
                    at};
    auto first = bytes_.begin() + static_cast<std::ptrdiff_t>(position_);
    auto end = first + static_cast<std::ptrdiff_t>(n);
    if (end[-1] != 0 || std::find(first, end - 1, 0) != end - 1)
      throw Problem{DiagnosticCode::malformed,
                    "memory string has invalid NUL termination", position_};
    std::string result(
        reinterpret_cast<const char *>(bytes_.data() + position_),
        static_cast<std::size_t>(n) - 1);
    position_ += static_cast<std::size_t>(n);
    return result;
  }

private:
  const Bytes &bytes_;
  ByteOrder order_;
  std::size_t position_ = 0;
};
std::string lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}
std::string address_string(std::uint64_t address) {
  std::ostringstream out;
  out << "0x" << std::hex << address;
  return out.str();
}
bool active_at(const Allocation &a, std::size_t prefix) {
  return a.allocated_event_count <= prefix &&
         (!a.ended_event_count || *a.ended_event_count > prefix);
}
bool matches(const Allocation &a, std::size_t prefix, const Filter &f) {
  if (a.allocated_event_count > prefix)
    return false;
  if (f.source && !(a.source == *f.source))
    return false;
  if (f.address && a.address != *f.address)
    return false;
  if (f.type_name && a.type_name != f.type_name)
    return false;
  if (f.file && a.file != f.file)
    return false;
  if (f.line && a.line != *f.line)
    return false;
  if (f.active_only && !active_at(a, prefix))
    return false;
  if (!f.text.empty()) {
    auto hay =
        lower(a.type_name.value_or("") + " " + a.file.value_or("") + " " +
              address_string(a.address) + " " + std::to_string(a.line));
    if (hay.find(lower(f.text)) == std::string::npos)
      return false;
  }
  return true;
}
Allocation at_prefix(Allocation a, std::size_t prefix) {
  if (a.ended_event_count && *a.ended_event_count > prefix) {
    a.ended_event_count.reset();
    a.ended_memory_index.reset();
    a.end_reason = EndReason::none;
  }
  return a;
}
void add(std::uint64_t &value, std::uint64_t amount) {
  if (amount > std::numeric_limits<std::uint64_t>::max() - value)
    throw std::overflow_error("memory statistics overflow");
  value += amount;
}
void allocate(Stats &s, std::uint64_t bytes) {
  add(s.allocated_bytes, bytes);
  add(s.current_bytes, bytes);
  ++s.allocation_count;
  ++s.current_allocations;
  s.peak_bytes = std::max(s.peak_bytes, s.current_bytes);
  s.peak_allocations = std::max(s.peak_allocations, s.current_allocations);
}
void release(Stats &s, std::uint64_t bytes, bool replaced) {
  if (bytes > s.current_bytes || !s.current_allocations)
    throw std::logic_error("memory index accounting mismatch");
  s.current_bytes -= bytes;
  --s.current_allocations;
  if (replaced)
    add(s.replaced_bytes, bytes);
  else {
    add(s.freed_bytes, bytes);
    ++s.free_count;
  }
}
using GroupKey = std::tuple<std::optional<std::string>,
                            std::optional<std::string>, std::uint32_t>;
GroupKey group_key(const Allocation &a, GroupBy by) {
  return {by == GroupBy::type || by == GroupBy::type_file_line ? a.type_name
                                                               : std::nullopt,
          by == GroupBy::type ? std::nullopt : a.file,
          by == GroupBy::file_line || by == GroupBy::type_file_line ? a.line
                                                                    : 0};
}
void csv_string(std::ostream &out, const std::string &value) {
  out << '"';
  for (char c : value) {
    if (c == '"')
      out << '"';
    out << c;
  }
  out << '"';
}
const char *end_name(EndReason r) {
  switch (r) {
  case EndReason::freed:
    return "freed";
  case EndReason::replaced_without_free:
    return "replaced_without_free";
  case EndReason::none:
    return "active";
  }
  return "active";
}
} // namespace

struct Index::Impl {
  struct State {
    std::map<std::uint32_t, std::string> strings;
    std::map<std::uint64_t, std::size_t> active;
    std::map<std::uint64_t, std::uint64_t> generations;
    bool stopped = false;
  };
  Options options;
  std::map<Source, State> sources;
  std::vector<Event> events_;
  std::vector<Allocation> allocations_;
  std::vector<Diagnostic> diagnostics_;
  std::size_t previous = 0, processed_packets = 0;
  bool seen = false, stopped_all = false, failed = false;
  const Session *session = nullptr;
  explicit Impl(Options settings) : options(settings) {}
  Snapshot snapshot(std::size_t prefix, GroupBy by, bool include_active) const;
  void parse(const std::vector<ChannelPacket> &packets, std::size_t from,
             std::size_t to) {
    if (stopped_all)
      return;
    for (std::size_t packet_index = from; packet_index < to; ++packet_index) {
      const auto &packet = packets[packet_index];
      if (packet.semantic != "memory event stream")
        continue;
      const Source source{packet.stream_id, packet.instance_id};
      auto &state = sources[source];
      auto diagnostic = [&](DiagnosticCode code, std::string message,
                            std::size_t offset) {
        diagnostics_.push_back(
            {code, source, packet.event_index, offset, std::move(message)});
      };
      if (seen && packet.event_index < previous) {
        diagnostic(
            DiagnosticCode::malformed,
            "memory channel packets are not ordered by outer event index", 0);
        stopped_all = true;
        break;
      }
      previous = packet.event_index;
      seen = true;
      if (state.stopped)
        continue;
      if (packet.event_index == std::numeric_limits<std::size_t>::max()) {
        diagnostic(DiagnosticCode::limit_exceeded,
                   "outer event index cannot form an event-count prefix", 0);
        state.stopped = true;
        continue;
      }
      const auto &bytes = packet.value.raw_bytes();
      if (bytes.size() > options.max_packet_bytes) {
        diagnostic(DiagnosticCode::limit_exceeded,
                   "memory packet exceeds byte limit", 0);
        state.stopped = true;
        continue;
      }
      Reader r(bytes, packet.value.byte_order);
      try {
        while (r.remaining()) {
          const auto offset = r.position();
          if (events_.size() >= options.max_memory_events)
            throw Problem{DiagnosticCode::limit_exceeded,
                          "memory event count exceeds limit", offset};
          const auto header = static_cast<std::uint16_t>(r.integer(2));
          const auto kind = header & 15;
          if (header & 0xc000u)
            throw Problem{DiagnosticCode::unsupported_event,
                          "memory header uses unsupported reserved bits",
                          offset};
          Event event;
          event.source = source;
          event.event_index = packet.event_index;
          event.payload_offset = offset;
          if (kind == 1) {
            event.kind = EventKind::string_table;
            event.string_value = r.string(options.max_string_bytes);
            event.string_handle = static_cast<std::uint32_t>(r.integer(4));
            const auto old = state.strings.find(event.string_handle);
            if (old != state.strings.end() && old->second != event.string_value)
              throw Problem{
                  DiagnosticCode::conflicting_string,
                  "memory string handle was redefined with different text",
                  offset};
            if (old == state.strings.end() &&
                state.strings.size() >= options.max_strings_per_source)
              throw Problem{DiagnosticCode::limit_exceeded,
                            "memory string table exceeds limit", offset};
            state.strings.emplace(event.string_handle, event.string_value);
            events_.push_back(std::move(event));
          } else if (kind == 2) {
            if (allocations_.size() >= options.max_allocations)
              throw Problem{DiagnosticCode::limit_exceeded,
                            "allocation history exceeds limit", offset};
            Allocation a;
            a.source = source;
            a.size = r.compressed(header, 10);
            a.type_handle = static_cast<std::uint32_t>(r.compressed(header, 6));
            a.file_handle = static_cast<std::uint32_t>(r.compressed(header, 8));
            a.line = static_cast<std::uint32_t>(r.compressed(header, 12));
            a.address = r.compressed(header, 4, true);
            if (!a.address)
              throw Problem{DiagnosticCode::malformed,
                            "SDK allocation events must have a nonzero address",
                            offset};
            auto type = state.strings.find(a.type_handle),
                 file = state.strings.find(a.file_handle);
            if (type != state.strings.end())
              a.type_name = type->second;
            else
              diagnostic(DiagnosticCode::undefined_string,
                         "allocation references undefined type handle " +
                             std::to_string(a.type_handle),
                         offset);
            if (file != state.strings.end())
              a.file = file->second;
            else
              diagnostic(DiagnosticCode::undefined_string,
                         "allocation references undefined file handle " +
                             std::to_string(a.file_handle),
                         offset);
            a.generation = state.generations[a.address]++;
            a.allocated_event_count = packet.event_index + 1;
            a.allocated_memory_index = events_.size();
            event.kind = EventKind::allocation;
            event.address = a.address;
            event.allocation_index = allocations_.size();
            auto old = state.active.find(a.address);
            if (old != state.active.end()) {
              event.replaced_allocation_index = old->second;
              auto &replaced = allocations_[old->second];
              replaced.ended_event_count = packet.event_index + 1;
              replaced.ended_memory_index = events_.size();
              replaced.end_reason = EndReason::replaced_without_free;
              diagnostic(DiagnosticCode::duplicate_allocation,
                         "address allocated again without an observed free: " +
                             address_string(a.address),
                         offset);
            }
            const auto index = allocations_.size();
            allocations_.push_back(std::move(a));
            events_.push_back(std::move(event));
            state.active[allocations_.back().address] = index;
          } else if (kind == 3) {
            event.kind = EventKind::deallocation;
            event.address = r.compressed(header, 4, true);
            const auto found = state.active.find(event.address);
            if (found == state.active.end())
              diagnostic(DiagnosticCode::unknown_free,
                         "free has no observed active allocation: " +
                             address_string(event.address),
                         offset);
            else {
              event.allocation_index = found->second;
              auto &a = allocations_[found->second];
              a.ended_event_count = packet.event_index + 1;
              a.ended_memory_index = events_.size();
              a.end_reason = EndReason::freed;
              state.active.erase(found);
            }
            events_.push_back(std::move(event));
          } else if (kind == 4)
            throw Problem{
                DiagnosticCode::unsupported_full_allocation,
                "FullAllocationEvent is an in-process SDK record with "
                "no PhysX 3.4 wire serializer",
                offset};
          else
            throw Problem{DiagnosticCode::unsupported_event,
                          "unknown memory event tag " + std::to_string(kind) +
                              " has no independent payload length",
                          offset};
        }
      } catch (const Problem &problem) {
        diagnostic(problem.code, problem.message, problem.offset);
        state.stopped = true;
      }
    }
  }
};
Index::Index(Options options) : impl_(std::make_unique<Impl>(options)) {}
Index::Index(const Session &session, Options options) : Index(options) {
  update(session);
}
Index::Index(const std::vector<ChannelPacket> &packets, Options options)
    : Index(options) {
  impl_->parse(packets, 0, packets.size());
  impl_->processed_packets = packets.size();
}
Index::~Index() = default;
Index::Index(Index &&) noexcept = default;
Index &Index::operator=(Index &&) noexcept = default;
const std::vector<Event> &Index::events() const noexcept {
  return impl_->events_;
}
const std::vector<Allocation> &Index::allocations() const noexcept {
  return impl_->allocations_;
}
const std::vector<Diagnostic> &Index::diagnostics() const noexcept {
  return impl_->diagnostics_;
}
void Index::update(const Session &session, std::size_t prefix) {
  if (impl_->failed)
    throw std::logic_error(
        "memory index update previously failed; rebuild required");
  if (impl_->session && impl_->session != &session)
    throw std::invalid_argument(
        "memory index cannot combine different sessions");
  if (prefix == std::numeric_limits<std::size_t>::max())
    prefix = session.event_count();
  if (prefix > session.event_count())
    throw std::out_of_range("memory update prefix exceeds session");
  impl_->session = &session;
  const auto &packets = session.channels();
  if (impl_->processed_packets > packets.size())
    throw std::invalid_argument("memory session channel history shrank");
  auto end = impl_->processed_packets;
  while (end < packets.size() && packets[end].event_index < prefix)
    ++end;
  try {
    impl_->parse(packets, impl_->processed_packets, end);
    impl_->processed_packets = end;
  } catch (...) {
    impl_->failed = true;
    throw;
  }
}

Snapshot Index::Impl::snapshot(std::size_t prefix, GroupBy by,
                               bool include_active) const {
  if (failed)
    throw std::logic_error("memory index update failed; rebuild required");
  Snapshot result;
  result.event_count = prefix;
  std::map<GroupKey, Stats> groups;
  for (const auto &e : events_) {
    if (e.event_index >= prefix)
      break;
    if (e.kind == EventKind::allocation) {
      if (e.replaced_allocation_index) {
        const auto &old = allocations_[*e.replaced_allocation_index];
        release(result.totals, old.size, true);
        release(groups[group_key(old, by)], old.size, true);
      }
      const auto &a = allocations_[*e.allocation_index];
      allocate(result.totals, a.size);
      allocate(groups[group_key(a, by)], a.size);
    } else if (e.kind == EventKind::deallocation) {
      if (e.allocation_index) {
        const auto &a = allocations_[*e.allocation_index];
        release(result.totals, a.size, false);
        release(groups[group_key(a, by)], a.size, false);
      } else {
        ++result.totals.free_count;
        ++result.totals.unknown_free_count;
      }
    }
  }
  if (include_active)
    for (const auto &a : allocations_)
      if (active_at(a, prefix))
        result.active.push_back(at_prefix(a, prefix));
  for (const auto &entry : groups)
    result.groups.push_back({std::get<0>(entry.first), std::get<1>(entry.first),
                             std::get<2>(entry.first), entry.second});
  for (const auto &d : diagnostics_)
    if (d.event_index < prefix) {
      result.diagnostics.push_back(d);
      result.complete = false;
    }
  return result;
}
Snapshot Index::snapshot_at_event(std::size_t prefix, GroupBy by) const {
  return impl_->snapshot(prefix, by, true);
}
Snapshot Index::summary_at_event(std::size_t prefix, GroupBy by) const {
  return impl_->snapshot(prefix, by, false);
}

std::vector<Allocation> Index::history_at_event(std::size_t prefix,
                                                Filter filter,
                                                std::size_t offset,
                                                std::size_t limit) const {
  if (impl_->failed)
    throw std::logic_error("memory index update failed; rebuild required");
  std::vector<Allocation> result;
  for (const auto &a : impl_->allocations_)
    if (matches(a, prefix, filter)) {
      if (offset) {
        --offset;
        continue;
      }
      if (result.size() == limit)
        break;
      result.push_back(at_prefix(a, prefix));
    }
  return result;
}
std::size_t Index::matching_count(std::size_t prefix, Filter filter) const {
  if (impl_->failed)
    throw std::logic_error("memory index update failed; rebuild required");
  std::size_t n = 0;
  for (const auto &a : impl_->allocations_)
    if (matches(a, prefix, filter))
      ++n;
  return n;
}
void Index::write_csv(std::ostream &out, std::size_t prefix,
                      Filter filter) const {
  if (impl_->failed)
    throw std::logic_error("memory index update failed; rebuild required");
  FormatGuard format(out);
  out << "stream_id,buffer_id,address,generation,size,type,file,line,allocated_"
         "event_count,ended_event_count,end_reason,type_resolved,file_"
         "resolved\r\n";
  for (const auto &record : impl_->allocations_)
    if (matches(record, prefix, filter)) {
      const auto a = at_prefix(record, prefix);
      out << a.source.stream_id << ',' << a.source.buffer_id << ','
          << address_string(a.address) << ',' << a.generation << ',' << a.size
          << ',';
      csv_string(out, a.type_name.value_or(""));
      out << ',';
      csv_string(out, a.file.value_or(""));
      out << ',' << a.line << ',' << a.allocated_event_count << ',';
      if (a.ended_event_count)
        out << *a.ended_event_count;
      out << ',' << end_name(a.end_reason) << ',' << a.type_name.has_value()
          << ',' << a.file.has_value() << "\r\n";
    }
  if (!out)
    throw std::runtime_error("unable to write memory CSV");
}

void Index::write_groups_csv(std::ostream &out, std::size_t prefix,
                             GroupBy by) const {
  const auto summary = summary_at_event(prefix, by);
  const char *name = by == GroupBy::type        ? "type"
                     : by == GroupBy::file      ? "file"
                     : by == GroupBy::file_line ? "file_line"
                                                : "type_file_line";
  FormatGuard format(out);
  out << "event_count,group_by,type,file,line,type_resolved,file_resolved,"
         "current_bytes,peak_bytes,allocated_bytes,freed_bytes,replaced_bytes,"
         "current_allocations,peak_allocations,allocation_count,free_count,"
         "unknown_free_count,complete\r\n";
  for (const auto &g : summary.groups) {
    const auto &s = g.stats;
    out << prefix << ',' << name << ',';
    csv_string(out, g.type_name.value_or(""));
    out << ',';
    csv_string(out, g.file.value_or(""));
    out << ',' << g.line << ',' << g.type_name.has_value() << ','
        << g.file.has_value() << ',' << s.current_bytes << ',' << s.peak_bytes
        << ',' << s.allocated_bytes << ',' << s.freed_bytes << ','
        << s.replaced_bytes << ',' << s.current_allocations << ','
        << s.peak_allocations << ',' << s.allocation_count << ','
        << s.free_count << ',' << s.unknown_free_count << ','
        << summary.complete << "\r\n";
  }
  if (!out)
    throw std::runtime_error("unable to write memory group CSV");
}
} // namespace pvd::memory
