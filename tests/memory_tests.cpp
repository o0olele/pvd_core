#include <iostream>
#include <pvd/memory.hpp>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace mem = pvd::memory;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T, class F> void rejects(F fn, const char *message) {
  try {
    fn();
  } catch (const T &) {
    return;
  }
  throw std::runtime_error(message);
}
// Independent little/big endian fixtures. Header constants follow the public
// SDK's bit table; logical-u32 code 3 deliberately stays four bytes.
struct Writer {
  ByteOrder order;
  Bytes bytes;
  explicit Writer(ByteOrder o = ByteOrder::little) : order(o) {}
  void u(std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          value >> (8 * (order == ByteOrder::little ? i : width - i - 1))));
  }
  void string(std::uint32_t handle, const std::string &text) {
    u(0x3ff1, 2);
    u(text.size() + 1, 4);
    bytes.insert(bytes.end(), text.begin(), text.end());
    u(0, 1);
    u(handle, 4);
  }
  void allocation(std::uint64_t addr, std::uint32_t size = 64,
                  std::uint32_t type = 1, std::uint32_t file = 2,
                  std::uint32_t line = 42, unsigned code = 3) {
    u(2u | (code << 4) | (code << 6) | (code << 8) | (code << 10) |
          (code << 12),
      2);
    const std::size_t w = code == 0 ? 1 : code == 1 ? 2 : 4;
    u(size, w);
    u(type, w);
    u(file, w);
    u(line, w);
    u(addr, code == 3 ? 8 : w);
  }
  void free(std::uint64_t addr, unsigned code = 3) {
    u(3u | (code << 4), 2);
    u(addr, code == 3 ? 8 : std::size_t(1) << code);
  }
};
ChannelPacket packet(Bytes bytes, std::size_t at = 0, std::uint64_t stream = 1,
                     std::uint64_t buffer = 10,
                     ByteOrder order = ByteOrder::little,
                     std::uint8_t pointer_width = 8) {
  PropertyValue v;
  v.bytes = std::make_shared<const Bytes>(std::move(bytes));
  v.byte_order = order;
  v.pointer_width = pointer_width;
  v.item_count = v.bytes->size();
  v.incoming_type = {"physx3", "PvdU8"};
  return {at, stream, buffer, "events", "memory event stream", std::move(v)};
}
ChannelPacket strings(std::size_t at = 0, ByteOrder order = ByteOrder::little) {
  Writer w(order);
  w.string(1, "Widget");
  w.string(2, "fixture.cpp");
  return packet(w.bytes, at, 1, 10, order);
}
bool diagnostic(const mem::Index &index, mem::DiagnosticCode code) {
  for (const auto &d : index.diagnostics())
    if (d.code == code)
      return true;
  return false;
}
void compressed_and_endian() {
  for (auto order : {ByteOrder::little, ByteOrder::big})
    for (auto ptr : {4u, 8u})
      for (unsigned code = 0; code != 4; ++code) {
        Writer w(order);
        w.allocation(code == 3   ? 0x123456789abcdef0ull
                     : code == 2 ? 0x12345678u
                     : code == 1 ? 0x1234
                                 : 0x34,
                     code == 0   ? 99
                     : code == 1 ? 12345
                                 : 0xfedcba98u,
                     1, 2,
                     code == 0   ? 12
                     : code == 1 ? 1000
                                 : 70000,
                     code);
        auto p =
            packet(w.bytes, 1, 1, 10, order, static_cast<std::uint8_t>(ptr));
        mem::Index i(std::vector<ChannelPacket>{strings(0, order), p});
        check(i.diagnostics().empty(), "valid compressed fields diagnosed");
        check(i.allocations().size() == 1, "allocation missing");
        const auto &a = i.allocations().front();
        check(a.address == (code == 3   ? 0x123456789abcdef0ull
                            : code == 2 ? 0x12345678u
                            : code == 1 ? 0x1234
                                        : 0x34),
              "compressed address mismatch");
        check(a.size == (code == 0   ? 99u
                         : code == 1 ? 12345u
                                     : 0xfedcba98u),
              "logical u32 width mismatch");
        check(a.line == (code == 0   ? 12u
                         : code == 1 ? 1000u
                                     : 70000u),
              "compressed line mismatch");
        check(a.type_name == "Widget" && a.file == "fixture.cpp",
              "dictionary resolution mismatch");
        check(w.bytes.size() == (code == 0   ? 7u
                                 : code == 1 ? 12u
                                 : code == 2 ? 22u
                                             : 26u),
              "fixture size incorrect");
      }
}
void prefix_history_and_stats() {
  Writer a;
  a.allocation(0x1000, 100, 1, 2, 10);
  a.allocation(0x2000, 50, 1, 2, 20);
  Writer b;
  b.free(0x1000);
  b.allocation(0x1000, 200, 1, 2, 10);
  Writer c;
  c.allocation(0x1000, 300, 1, 2, 10);
  c.free(0x2000);
  c.free(0x9999);
  mem::Index i(std::vector<ChannelPacket>{
      strings(), packet(a.bytes, 2), packet(b.bytes, 5), packet(c.bytes, 8)});
  check(i.snapshot_at_event(2).active.empty(), "future packet leaked");
  auto first = i.snapshot_at_event(3);
  check(first.active.size() == 2 && first.totals.current_bytes == 150 &&
            first.totals.peak_bytes == 150 && first.complete,
        "first state incorrect");
  auto h = i.history_at_event(3);
  check(h.size() == 2 && !h[0].ended_event_count &&
            h[0].end_reason == mem::EndReason::none,
        "future end leaked");
  auto second = i.snapshot_at_event(6);
  check(second.totals.current_bytes == 250 && second.totals.peak_bytes == 250 &&
            second.totals.allocated_bytes == 350 &&
            second.totals.freed_bytes == 100,
        "reuse statistics incorrect");
  check(i.allocations()[2].generation == 1, "address reuse generation missing");
  auto last = i.snapshot_at_event(9);
  check(last.active.size() == 1 && last.active[0].generation == 2 &&
            last.active[0].size == 300,
        "replacement lifecycle incorrect");
  check(last.totals.current_bytes == 300 && last.totals.peak_bytes == 350 &&
            last.totals.allocated_bytes == 650 &&
            last.totals.freed_bytes == 150 && last.totals.replaced_bytes == 200,
        "cumulative/peak/replaced bytes incorrect");
  check(last.totals.allocation_count == 4 && last.totals.free_count == 3 &&
            last.totals.unknown_free_count == 1 && !last.complete,
        "free counters incorrect");
  check(last.groups.size() == 2, "line grouping lost");
  auto grouped = i.snapshot_at_event(9, mem::GroupBy::type);
  check(grouped.groups.size() == 1 &&
            grouped.groups[0].stats.current_bytes == 300,
        "type grouping incorrect");
  mem::Filter active;
  active.active_only = true;
  check(i.matching_count(9, active) == 1, "active filter incorrect");
  mem::Filter f;
  f.address = 0x1000;
  check(i.matching_count(9, f) == 3, "history address filter incorrect");
  f.text = "WIDGET";
  check(i.history_at_event(9, f, 1, 1)[0].generation == 1,
        "page/search incorrect");
  check(i.history_at_event(9, f, 0, 0).empty() &&
            i.history_at_event(9, f, 99, 1).empty(),
        "zero/outside pagination incorrect");
  // All inner events in a packet share its outer event-count boundary.
  Writer both;
  both.allocation(123, 7);
  both.free(123);
  mem::Index same(std::vector<ChannelPacket>{strings(), packet(both.bytes, 2)});
  check(same.snapshot_at_event(3).active.empty() &&
            same.snapshot_at_event(3).totals.peak_bytes == 7,
        "same-packet allocation/free mismatch");
  check(same.allocations()[0].allocated_memory_index <
            *same.allocations()[0].ended_memory_index,
        "inner order missing");
}
void source_isolation_and_csv() {
  Writer one;
  one.string(1, "T,\"\nUTF8\xe5\x90\x8d");
  one.string(2, "C:\\a,b.cpp");
  one.allocation(42, 5);
  Writer two;
  two.string(1, "Other");
  two.string(2, "");
  two.allocation(42, 6);
  Writer free;
  free.free(42);
  mem::Index i(std::vector<ChannelPacket>{packet(one.bytes, 0),
                                          packet(two.bytes, 1, 2, 10),
                                          packet(free.bytes, 2, 1, 10)});
  check(i.snapshot_at_event(3).active.size() == 1 &&
            i.snapshot_at_event(3).active[0].source.stream_id == 2,
        "cross-source address collision");
  check(i.allocations()[0].generation == 0 &&
            i.allocations()[1].generation == 0,
        "cross-source generation collision");
  std::ostringstream csv;
  i.write_csv(csv, 1);
  check(csv.str().find("\"T,\"\"\nUTF8") != std::string::npos &&
            csv.str().find("C:\\a,b.cpp") != std::string::npos,
        "CSV escape/UTF8 missing");
  check(csv.str().find(",active,1,1\r\n") != std::string::npos,
        "CSV exposed future end");
  mem::Filter f;
  f.source = mem::Source{2, 10};
  check(i.matching_count(3, f) == 1, "source filter mismatch");
  // Non-memory semantic must remain uninterpreted.
  auto ignored = packet({0xff}, 0);
  ignored.semantic = "render event stream";
  mem::Index none(std::vector<ChannelPacket>{ignored});
  check(none.events().empty() && none.diagnostics().empty(),
        "other channel parsed as memory");
}
void corruption_and_limits() {
  Writer allocation;
  allocation.allocation(0x123456789, 80);
  for (std::size_t n = 1; n < allocation.bytes.size(); ++n) {
    Bytes tail(allocation.bytes.begin(),
               allocation.bytes.begin() + static_cast<std::ptrdiff_t>(n));
    mem::Index i(std::vector<ChannelPacket>{strings(), packet(tail, 1),
                                            packet(allocation.bytes, 2)});
    check(diagnostic(i, mem::DiagnosticCode::malformed) &&
              i.allocations().empty(),
          "truncated event accepted or damaged source resumed");
    check(i.snapshot_at_event(1).complete && !i.snapshot_at_event(2).complete,
          "future corruption leaked backwards");
  }
  for (auto tag : {0u, 4u, 5u, 15u, 0x4001u}) {
    Writer w;
    w.u(tag, 2);
    mem::Index i(std::vector<ChannelPacket>{packet(w.bytes)});
    check(diagnostic(i, tag == 4
                            ? mem::DiagnosticCode::unsupported_full_allocation
                            : mem::DiagnosticCode::unsupported_event),
          "unknown/full/reserved tag not diagnosed");
  }
  Writer missing;
  missing.allocation(7);
  mem::Index m(std::vector<ChannelPacket>{packet(missing.bytes)});
  check(m.allocations().size() == 1 && !m.allocations()[0].type_name &&
            !m.allocations()[0].file && m.diagnostics().size() == 2,
        "undefined string handling incorrect");
  Writer conflict;
  conflict.string(1, "wrong");
  conflict.allocation(1);
  mem::Index c(
      std::vector<ChannelPacket>{strings(), packet(conflict.bytes, 1)});
  check(diagnostic(c, mem::DiagnosticCode::conflicting_string) &&
            c.allocations().empty(),
        "string redefine accepted");
  Writer bad;
  bad.u(1, 2);
  bad.u(2, 4);
  bad.u('a', 1);
  bad.u('x', 1);
  bad.u(1, 4);
  mem::Index nul(std::vector<ChannelPacket>{packet(bad.bytes)});
  check(diagnostic(nul, mem::DiagnosticCode::malformed),
        "unterminated string accepted");
  for (std::size_t n = 1; n < 14; ++n) {
    Writer w;
    w.string(1, "abc");
    w.bytes.resize(n);
    mem::Index i(std::vector<ChannelPacket>{packet(w.bytes)});
    check(diagnostic(i, mem::DiagnosticCode::malformed),
          "truncated string accepted");
  }
  Writer goodbad;
  goodbad.allocation(1);
  goodbad.u(5, 2);
  mem::Index prefix(
      std::vector<ChannelPacket>{strings(), packet(goodbad.bytes, 1)});
  check(prefix.allocations().size() == 1 &&
            !prefix.snapshot_at_event(2).complete,
        "valid inner prefix lost");
  mem::Options o;
  o.max_packet_bytes = 1;
  mem::Index pb(std::vector<ChannelPacket>{strings()}, o);
  check(diagnostic(pb, mem::DiagnosticCode::limit_exceeded),
        "packet bound ignored");
  o = {};
  o.max_string_bytes = 2;
  mem::Index sb(std::vector<ChannelPacket>{strings()}, o);
  check(diagnostic(sb, mem::DiagnosticCode::limit_exceeded),
        "string bound ignored");
  o = {};
  o.max_strings_per_source = 1;
  mem::Index tb(std::vector<ChannelPacket>{strings()}, o);
  check(tb.events().size() == 1 &&
            diagnostic(tb, mem::DiagnosticCode::limit_exceeded),
        "dictionary bound ignored");
  o = {};
  o.max_memory_events = 2;
  mem::Index eb(
      std::vector<ChannelPacket>{strings(), packet(allocation.bytes, 1)}, o);
  check(eb.events().size() == 2 && eb.allocations().empty() &&
            diagnostic(eb, mem::DiagnosticCode::limit_exceeded),
        "event bound ignored");
  o = {};
  o.max_allocations = 0;
  mem::Index ab(
      std::vector<ChannelPacket>{strings(), packet(allocation.bytes, 1)}, o);
  check(diagnostic(ab, mem::DiagnosticCode::limit_exceeded),
        "allocation bound ignored");
  mem::Index unordered(
      std::vector<ChannelPacket>{strings(4), packet(allocation.bytes, 3)});
  check(diagnostic(unordered, mem::DiagnosticCode::malformed),
        "out-of-order input accepted");
}
void summary_and_group_csv() {
  Writer w;
  w.string(1, "T,\"\nUTF8\xe5\x90\x8d");
  w.string(2, "source,quoted.cpp");
  w.allocation(0x1000, 0xffffffffu);
  w.allocation(0x2000, 0xffffffffu);
  mem::Index i(std::vector<ChannelPacket>{packet(w.bytes, 5)});
  for (auto by : {mem::GroupBy::type, mem::GroupBy::file,
                  mem::GroupBy::file_line, mem::GroupBy::type_file_line}) {
    const auto full = i.snapshot_at_event(6, by),
               summary = i.summary_at_event(6, by);
    check(full.active.size() == 2 && summary.active.empty(),
          "summary copied active allocation list");
    check(summary.complete == full.complete &&
              summary.event_count == full.event_count &&
              summary.diagnostics.size() == full.diagnostics.size() &&
              summary.groups.size() == full.groups.size(),
          "summary metadata differs");
    check(summary.totals.current_bytes == 8589934590ull &&
              summary.totals.peak_bytes == full.totals.peak_bytes,
          "summary total changed");
    check(summary.groups[0].stats.allocated_bytes ==
                  full.groups[0].stats.allocated_bytes &&
              summary.groups[0].stats.current_allocations == 2,
          "summary group differs");
  }
  std::ostringstream csv;
  csv << std::hex << std::boolalpha;
  const auto previous_flags = csv.flags();
  i.write_groups_csv(csv, 6);
  check(csv.flags() == previous_flags,
        "group CSV changed caller stream format");
  check(csv.str().find("\"T,\"\"\nUTF8") != std::string::npos &&
            csv.str().find("\"source,quoted.cpp\"") != std::string::npos,
        "group CSV quoting/UTF8 failed");
  check(
      csv.str().find(",8589934590,8589934590,8589934590,0,0,2,2,2,0,0,1\r\n") !=
          std::string::npos,
      "group CSV integer precision/flags lost");
  std::ostringstream before;
  i.write_groups_csv(before, 5);
  check(before.str().find("type_file_line,\"") == std::string::npos,
        "group CSV leaked future data");
  Writer bad;
  bad.free(0x9999);
  mem::Index incomplete(
      std::vector<ChannelPacket>{packet(w.bytes, 5), packet(bad.bytes, 6)});
  std::ostringstream after;
  incomplete.write_groups_csv(after, 7, mem::GroupBy::file);
  check(after.str().find("7,file,\"\",\"source,quoted.cpp\",0,0,1,") !=
                std::string::npos &&
            after.str().find(",0,0,0\r\n") != std::string::npos,
        "group dimensions/completeness lost");
  check(incomplete.summary_at_event(6).complete &&
            !incomplete.summary_at_event(7).complete,
        "summary diagnostics leaked future");
}
void incremental() {
  std::vector<pvd::Event> events;
  auto emit = [&](Payload p) { events.push_back({1, 0, 0, std::move(p)}); };
  emit(StringHandleEvent{"test", 1});
  emit(StringHandleEvent{"MemoryBuffer", 2});
  emit(StringHandleEvent{"events", 3});
  emit(StringHandleEvent{"memory event stream", 4});
  emit(StringHandleEvent{"physx3", 5});
  emit(StringHandleEvent{"PvdU8", 6});
  emit(CreateClass{{1, 2}});
  emit(CreateProperty{{1, 2}, 3, 4, {5, 6}, 2, {}});
  emit(CreateInstance{{1, 2}, 10});
  auto p0 = strings().value.raw_bytes();
  emit(SetPropertyValue{
      10, 3, {5, 6}, static_cast<std::uint32_t>(p0.size()), p0});
  Writer a;
  a.allocation(100);
  emit(SetPropertyValue{
      10, 3, {5, 6}, static_cast<std::uint32_t>(a.bytes.size()), a.bytes});
  Writer b;
  b.free(100);
  b.allocation(100, 90);
  emit(SetPropertyValue{
      10, 3, {5, 6}, static_cast<std::uint32_t>(b.bytes.size()), b.bytes});
  Session s(StreamHeader{}, events);
  mem::Index whole(s), live;
  for (std::size_t n = 0; n <= s.event_count(); ++n) {
    live.update(s, n);
    live.update(s, n);
    auto actual = live.snapshot_at_event(n),
         expected = whole.snapshot_at_event(n);
    check(actual.totals.current_bytes == expected.totals.current_bytes &&
              actual.totals.allocated_bytes ==
                  expected.totals.allocated_bytes &&
              actual.active.size() == expected.active.size(),
          "incremental differs from full decode");
  }
  check(live.events().size() == whole.events().size() &&
            live.allocations().size() == 2 &&
            live.allocations()[1].generation == 1,
        "incremental duplicated or lost state");
  live.update(s, 0);
  check(live.events().size() == whole.events().size(),
        "older prefix rewound decoder");
  rejects<std::out_of_range>([&] { live.update(s, s.event_count() + 1); },
                             "invalid prefix accepted");
  Session other(StreamHeader{});
  rejects<std::invalid_argument>([&] { live.update(other); },
                                 "different session combined");
  // Live Session append, not just prefix-limited access to a finished Session.
  Session stream(StreamHeader{});
  mem::Index appended;
  for (const auto &e : events) {
    stream.append({e});
    appended.update(stream);
  }
  stream.finish();
  appended.update(stream);
  check(appended.events().size() == whole.events().size() &&
            appended.snapshot_at_event(events.size()).totals.current_bytes ==
                90,
        "live append state lost");
}
} // namespace
int main() {
  try {
    compressed_and_endian();
    prefix_history_and_stats();
    source_isolation_and_csv();
    corruption_and_limits();
    summary_and_group_csv();
    incremental();
    std::cout << "memory tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
