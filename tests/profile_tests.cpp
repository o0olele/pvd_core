#include "pvd/profile.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace cpu = pvd::profile;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F &&f) {
  try {
    f();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("expected rejection");
}
struct Writer {
  ByteOrder order;
  Bytes bytes;
  explicit Writer(ByteOrder o = ByteOrder::little) : order(o) {}
  void u(std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          value >> ((order == ByteOrder::little ? i : width - 1 - i) * 8)));
  }
  void header(unsigned tag, unsigned flags, std::uint16_t id = 1) {
    u(tag, 1);
    u(flags, 1);
    u(id, 2);
  }
  void full(unsigned tag, std::uint64_t timestamp, std::uint16_t id = 1,
            std::uint32_t thread = 7,
            std::uint64_t context = 0x1122334455667788ull,
            unsigned time_code = 3, unsigned context_code = 3) {
    header(tag, time_code | (context_code << 2), id);
    u(thread, 4);
    u(context, std::size_t(1) << context_code);
    u(4, 1);
    u(2, 1);
    u(timestamp, std::size_t(1) << time_code);
  }
  void relative(unsigned tag, std::uint64_t timestamp, std::uint16_t id = 1,
                unsigned code = 0) {
    header(tag, code, id);
    u(timestamp, std::size_t(1) << code);
  }
  void value(std::uint64_t value, std::uint16_t id = 3) {
    header(5, 15, id);
    u(value, 8);
    u(42, 8);
    u(9, 4);
  }
  void gpu(std::uint32_t repeated = 3) {
    header(6, 0);
    u(123, 8);
    u(0x3f800000, 4);
    u(3, 4);
    u(0xabcdef, 3);
    u(repeated, 4);
    u(7, 4);
  }
};
StreamHeader clock_header() {
  StreamHeader h;
  h.timestamp_numerator = 10;
  h.timestamp_denominator = 2;
  return h;
}
ChannelPacket packet(Bytes bytes, ByteOrder order = ByteOrder::little,
                     std::size_t event = 10, std::uint64_t zone = 10) {
  ChannelPacket p;
  p.event_index = event;
  p.instance_id = zone;
  p.stream_id = 8;
  p.semantic = "profile event stream";
  p.property_name = "events";
  p.value.incoming_type = {"physx3", "PvdU8"};
  p.value.item_count = bytes.size();
  p.value.byte_order = order;
  p.value.bytes = std::make_shared<const Bytes>(std::move(bytes));
  return p;
}
void names(cpu::Index &index) {
  index.register_zone({10, 0}, "Zone,\"one\"");
  for (unsigned i = 1; i <= 6; ++i)
    index.register_event({10, 0}, static_cast<std::uint16_t>(i),
                         "Event " + std::to_string(i));
}
bool diagnostic(const cpu::Index &i, cpu::DiagnosticCode code) {
  for (const auto &d : i.diagnostics())
    if (d.code == code)
      return true;
  return false;
}
void pairing(ByteOrder order) {
  cpu::Index index(clock_header());
  names(index);
  Writer a(order);
  a.full(1, 1000);
  a.relative(3, 10, 2);
  a.relative(4, 20, 2);
  index.append_packet(packet(a.bytes, order, 10));
  check(index.intervals().size() == 1 && index.statistics().open_scopes == 1,
        "first packet inner pairing");
  Writer b(order);
  b.full(2, 1100);
  b.full(1, 1200, 1, 8);
  b.relative(4, 0);
  index.append_packet(packet(b.bytes, order, 20));
  index.finish();
  check(index.events().size() == 6 && index.intervals().size() == 3,
        "cross packet pairing count");
  const auto &inner = index.intervals()[0];
  const auto &outer = index.intervals()[1];
  check(inner.duration_ticks == 20 && inner.depth == 1 &&
            inner.duration_nanoseconds == 100.,
        "nested duration and calibration");
  check(outer.duration_ticks == 100 && outer.depth == 0 &&
            outer.duration_nanoseconds == 500.,
        "cross packet absolute timestamp");
  check(outer.thread_id == 7 && outer.context_id == 0x1122334455667788ull &&
            outer.cpu_id == 2 && outer.thread_priority == 4,
        "full context retained");
  check(index.intervals()[2].duration_ticks == 0 &&
            index.intervals()[2].thread_id == 8,
        "valid zero interval");
  check(index.statistics().complete && index.statistics().finished,
        "valid packet complete");
  cpu::Query q;
  q.end_event_count = 20;
  check(index.query(q).size() == 1, "future stop excluded by source prefix");
  q.begin_event_count = 20;
  q.end_event_count = 21;
  check(index.query(q).size() == 2, "stop event prefix filter");
  q = {};
  q.thread_id = 7;
  check(index.query(q).size() == 2, "thread filter");
  q.context_id = 1;
  check(index.query(q).empty(), "context filter");
  q = {};
  q.begin_ticks = 1011;
  q.end_ticks = 1029;
  check(index.query(q).size() == 2, "time overlap filter");
  q.begin_ticks = 1029;
  q.end_ticks = 1029;
  check(index.query(q).empty(), "empty tick range");
  q = {};
  q.begin_ticks = 1030;
  check(index.query(q).size() == 2, "half-open interval end excluded");
  q = {};
  q.text = "Event 2";
  check(index.query(q).size() == 1 && index.aggregate(q).size() == 1,
        "name search applies to query and aggregation");
  std::ostringstream filtered;
  index.write_csv(filtered, q);
  check(filtered.str().find("Event 1") == std::string::npos &&
            filtered.str().find("Event 2") != std::string::npos,
        "name search applies to CSV");
  q.text = "event 2";
  check(index.query(q).empty(), "name search case-sensitive");
  q.text = "Zone,";
  check(index.query(q).size() == 3, "zone search");
  auto rows = index.aggregate({}, true);
  check(rows.size() == 3, "aggregate thread grouping");
  auto all = index.aggregate();
  check(all.size() == 2 && all[0].count == 2 && all[0].total_ticks == 100 &&
            all[0].mean_nanoseconds == 250.,
        "aggregate inclusive duration");
  std::ostringstream csv;
  index.write_csv(csv);
  check(csv.str().find("\"Zone,\"\"one\"\"\"") != std::string::npos,
        "CSV escaping");
  rejects([&] { index.append_packet(packet({})); });
  index.finish();
}
void compression(ByteOrder order) {
  for (unsigned code = 0; code < 4; ++code) {
    cpu::Index i(clock_header());
    names(i);
    Writer w(order);
    const std::uint64_t context =
        code == 3
            ? 0xabcdef1234567890ull
            : ((std::uint64_t(1) << (8 * (std::size_t(1) << code) - 1)) - 1);
    w.full(1, 20, 1, 7, context, code, code);
    w.relative(4, code == 3 ? 30 : 10, 1, code);
    i.append_packet(packet(w.bytes, order));
    i.finish();
    check(i.intervals().size() == 1 && i.intervals()[0].context_id == context &&
              i.intervals()[0].duration_ticks == 10,
          "all compression widths");
  }
  cpu::Index i(clock_header());
  names(i);
  Writer w(order);
  w.value(70000);
  w.value(std::numeric_limits<std::uint64_t>::max());
  w.gpu();
  w.full(1, 100);
  w.relative(4, 7);
  i.append_packet(packet(w.bytes, order));
  i.finish();
  check(i.events().size() == 5 && i.events()[0].value == 70000 &&
            i.events()[0].sdk_legacy_value == 4464,
        "raw and SDK legacy value views");
  check(i.events()[1].value == -1 && i.events()[1].sdk_legacy_value == -1 &&
            !i.events()[1].timestamp_ticks,
        "signed value no timestamp");
  check(i.events()[2].kind == cpu::Kind::gpu &&
            i.events()[2].gpu_payload_bytes == 3 &&
            i.events()[2].gpu_version == 7,
        "GPU payload bounded skip");
  check(i.intervals().size() == 1 && i.intervals()[0].duration_ticks == 7,
        "CPU decode continues after GPU");
  check(diagnostic(i, cpu::DiagnosticCode::unsupported_gpu) &&
            diagnostic(i, cpu::DiagnosticCode::legacy_value_truncation) &&
            !i.statistics().complete,
        "unsupported data explicitly diagnosed");
}
void crossing() {
  cpu::Index index(clock_header());
  names(index);
  Writer w;
  w.full(1, 100, 1);
  w.relative(3, 10, 2);
  w.relative(4, 10, 1);
  w.relative(3, 10, 3);
  w.relative(4, 10, 2);
  w.relative(4, 10, 3);
  index.append_packet(packet(w.bytes));
  index.finish();
  check(index.intervals().size() == 3 && index.intervals()[0].depth == 0 &&
            index.intervals()[1].depth == 1 && index.intervals()[2].depth == 0,
        "crossing scopes reuse vacant display lane without colliding");
}
void corrupt() {
  Writer w;
  w.full(1, 100);
  const auto start_size = w.bytes.size();
  w.relative(4, 10);
  for (std::size_t cut = 1; cut < w.bytes.size(); ++cut) {
    cpu::Index i(clock_header());
    names(i);
    auto bytes = w.bytes;
    bytes.resize(cut);
    i.append_packet(packet(bytes));
    i.finish();
    check(i.intervals().empty(),
          "truncated record must not fabricate interval");
    check(diagnostic(i, cut == start_size
                            ? cpu::DiagnosticCode::unclosed_start
                            : cpu::DiagnosticCode::truncated_packet),
          "truncation diagnosed");
  }
  cpu::Index i(clock_header());
  names(i);
  Writer a;
  a.full(1, 100);
  a.relative(4, 10);
  a.full(1, 200, 2);
  a.header(255, 0);
  i.append_packet(packet(a.bytes));
  Writer b;
  b.full(2, 300, 2);
  i.append_packet(packet(b.bytes, ByteOrder::little, 20));
  i.finish();
  check(i.intervals().size() == 1 &&
            diagnostic(i, cpu::DiagnosticCode::unknown_event) &&
            diagnostic(i, cpu::DiagnosticCode::abandoned_start) &&
            diagnostic(i, cpu::DiagnosticCode::unmatched_stop),
        "corrupt packet keeps complete prefix and abandons unsafe pairing");
  cpu::Index relative(clock_header());
  names(relative);
  Writer r;
  r.full(1, 100);
  relative.append_packet(packet(r.bytes));
  Writer r2;
  r2.relative(4, 10);
  relative.append_packet(packet(r2.bytes, ByteOrder::little, 20));
  check(diagnostic(relative, cpu::DiagnosticCode::relative_without_context),
        "context never leaks across packets");
  cpu::Index overflow(clock_header());
  names(overflow);
  Writer o;
  o.full(1, UINT64_MAX);
  o.relative(4, 1);
  overflow.append_packet(packet(o.bytes));
  check(diagnostic(overflow, cpu::DiagnosticCode::timestamp_overflow),
        "timestamp overflow");
  cpu::Index backward(clock_header());
  names(backward);
  Writer back;
  back.full(1, 100);
  back.relative(4, 50, 1, 3);
  backward.append_packet(packet(back.bytes));
  check(backward.intervals().empty() &&
            diagnostic(backward, cpu::DiagnosticCode::backwards_interval),
        "negative duration rejected");
  cpu::Index gpu(clock_header());
  names(gpu);
  Writer g;
  g.gpu(5);
  gpu.append_packet(packet(g.bytes));
  check(diagnostic(gpu, cpu::DiagnosticCode::malformed_packet),
        "GPU repeated length validated");
  cpu::Index wrong(clock_header());
  auto p = packet({});
  p.value.item_count = 1;
  wrong.append_packet(p);
  check(diagnostic(wrong, cpu::DiagnosticCode::malformed_packet),
        "channel array size checked");
  cpu::Index uncalibrated;
  Writer pair;
  pair.full(1, 1);
  pair.relative(4, 1);
  uncalibrated.append_packet(packet(pair.bytes));
  check(!uncalibrated.intervals()[0].duration_nanoseconds &&
            diagnostic(uncalibrated, cpu::DiagnosticCode::invalid_clock) &&
            diagnostic(uncalibrated, cpu::DiagnosticCode::missing_zone),
        "unknown clock and metadata never assumed");
}
void limits() {
  cpu::Options opt;
  opt.max_events = 1;
  cpu::Index i(clock_header(), opt);
  names(i);
  Writer w;
  w.full(1, 100);
  w.relative(4, 10);
  i.append_packet(packet(w.bytes));
  check(i.statistics().saturated && i.events().size() == 1 &&
            i.intervals().empty() && !i.statistics().open_scopes,
        "event budget stops without false span");
  opt = {};
  opt.max_packet_bytes = 1;
  cpu::Index bytes(clock_header(), opt);
  bytes.append_packet(packet({1, 2}));
  check(bytes.statistics().saturated && bytes.events().empty(),
        "packet bytes budget");
  opt = {};
  opt.max_open_scopes = 1;
  cpu::Index opened(clock_header(), opt);
  Writer two;
  two.full(1, 1);
  two.relative(3, 1, 2);
  opened.append_packet(packet(two.bytes));
  check(opened.statistics().saturated, "open scope budget");
  opt = {};
  opt.max_intervals = 1;
  cpu::Index intervals(clock_header(), opt);
  Writer spans;
  spans.full(1, 1);
  spans.relative(4, 1);
  spans.relative(3, 1);
  spans.relative(4, 1);
  intervals.append_packet(packet(spans.bytes));
  check(intervals.statistics().saturated && intervals.intervals().size() == 1,
        "interval budget");
  opt = {};
  opt.max_diagnostics = 1;
  cpu::Index diagnostics(clock_header(), opt);
  diagnostics.append_packet(packet(w.bytes));
  check(diagnostics.diagnostics().size() == 1 &&
            diagnostics.statistics().suppressed_diagnostics > 0,
        "diagnostics budget");
  opt = {};
  opt.max_zones = 1;
  cpu::Index zones(clock_header(), opt);
  zones.register_zone({1, 0}, "a");
  zones.register_zone({2, 0}, "b");
  check(zones.statistics().saturated && zones.zones().size() == 1,
        "zone budget");
  opt = {};
  opt.max_event_names = 1;
  cpu::Index names_index(clock_header(), opt);
  names_index.register_event({1, 0}, 1, "a");
  names_index.register_event({1, 0}, 2, "b");
  check(names_index.statistics().saturated, "name budget");
  opt = {};
  opt.max_name_bytes = 1;
  cpu::Index length(clock_header(), opt);
  length.register_zone({1, 0}, "abc");
  check(length.statistics().saturated, "name byte budget");
  opt = {};
  opt.max_events = 0;
  rejects([&] { cpu::Index invalid(clock_header(), opt); });
  cpu::Index ignored(clock_header());
  auto p = packet({1, 2});
  p.semantic = "unrelated";
  ignored.append_packet(p);
  check(ignored.statistics().profile_packets == 0 &&
            ignored.statistics().indexed_event_count == 0,
        "unrelated packet ignored");
}
struct Fixture {
  StreamHeader header = clock_header();
  std::vector<pvd::Event> events;
  std::map<std::string, std::uint32_t> handles;
  template <class T> void emit(T value) {
    events.push_back({1, events.size() * 100, events.size() * 100,
                      Payload{std::move(value)}});
  }
  std::uint32_t str(const std::string &s) {
    auto f = handles.find(s);
    if (f != handles.end())
      return f->second;
    auto id = static_cast<std::uint32_t>(handles.size() + 1);
    handles.emplace(s, id);
    emit(StringHandleEvent{s, id});
    return id;
  }
  WireName name(const char *ns, const char *n) {
    auto a = str(ns), b = str(n);
    return {a, b};
  }
  void setup() {
    auto cls = name("_debugger_", "PxProfileZone");
    emit(CreateClass{cls});
    auto prop = str("events"), semantic = str("profile event stream");
    auto type = name("physx3", "PvdU8");
    emit(CreateProperty{cls, prop, semantic, type, 2, {}});
    announce();
  }
  void announce() {
    emit(AddProfileZone{10, "Fixture"});
    emit(CreateInstance{name("_debugger_", "PxProfileZone"), 10});
    emit(AddProfileZoneEvent{10, "Task", 1, true});
  }
  void data(const Bytes &bytes) {
    auto prop = str("events");
    auto type = name("physx3", "PvdU8");
    emit(SetPropertyValue{10, prop, type,
                          static_cast<std::uint32_t>(bytes.size()), bytes});
  }
  std::vector<pvd::Event> take() {
    auto out = std::move(events);
    events.clear();
    return out;
  }
};
void incremental() {
  Fixture f;
  f.setup();
  Writer start;
  start.full(1, 100);
  f.data(start.bytes);
  Session session(f.header);
  session.append(f.take());
  cpu::Index i;
  i.update(session);
  check(i.events().size() == 1 && i.statistics().open_scopes == 1,
        "live session first prefix");
  Writer stop;
  stop.full(2, 200);
  f.data(stop.bytes);
  session.append(f.take());
  i.update(session);
  check(i.intervals().size() == 1 &&
            i.intervals()[0].duration_nanoseconds == 500.,
        "live default index adopts Session clock");
  auto count = session.event_count();
  f.data(start.bytes);
  f.emit(DestroyInstance{10});
  f.announce();
  f.data(stop.bytes);
  session.append(f.take());
  i.update(session);
  check(i.intervals().size() == 1 && i.zones().count({10, 1}) &&
            diagnostic(i, cpu::DiagnosticCode::abandoned_start) &&
            diagnostic(i, cpu::DiagnosticCode::unmatched_stop),
        "ID reuse does not pair across zone generations");
  rejects([&] { i.update(session, count); });
  Session other(f.header);
  rejects([&] { i.update(other); });
  session.finish();
  i.finish();
  rejects([&] { i.update(session); });
  cpu::Index offline(session);
  check(offline.statistics().finished && offline.intervals().size() == 1,
        "offline Session constructor");
}
std::vector<std::string> split(const std::string &line) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  for (;;) {
    auto end = line.find(',', begin);
    result.push_back(line.substr(begin, end - begin));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return result;
}
void sdk(const char *capture, const char *expected) {
  auto session = Session::load_file(capture);
  cpu::Index index(session);
  std::ifstream file(expected);
  check(bool(file), "open SDK profile expected CSV");
  std::map<std::uint64_t, std::vector<const cpu::Event *>> actual;
  for (const auto &event : index.events())
    actual[event.source.zone.id].push_back(&event);
  std::map<std::uint64_t, std::size_t> cursor;
  std::size_t expected_events = 0, expected_intervals = 0;
  std::uint64_t frequency = 0;
  using Key =
      std::tuple<std::uint64_t, std::uint16_t, std::uint32_t, std::uint64_t>;
  std::map<Key, std::vector<std::uint64_t>> open;
  std::string line;
  while (std::getline(file, line)) {
    auto row = split(line);
    if (row.empty() || row[0] == "kind")
      continue;
    if (row[0] == "clock") {
      frequency = std::stoull(row.at(2));
      continue;
    }
    check(row.size() == 9, "SDK oracle CSV schema");
    auto zone = std::stoull(row[1]);
    auto &position = cursor[zone];
    check(position < actual[zone].size(), "SDK oracle event exists");
    const auto &value = *actual[zone][position++];
    ++expected_events;
    auto id = static_cast<std::uint16_t>(std::stoul(row[2]));
    auto thread = static_cast<std::uint32_t>(std::stoul(row[3]));
    auto context = std::stoull(row[4]);
    check(value.event_id == id && value.thread_id == thread &&
              value.context_id == context,
          "SDK oracle event context matches");
    check(index.zone_name(value.source.zone).find("Fixture") !=
                  std::string::npos &&
              index.event_name(value.source.zone, id).front() != '#',
          "SDK zone and event names mapped");
    if (row[0] == "value") {
      check(value.kind == cpu::Kind::value &&
                value.sdk_legacy_value == std::stoll(row[8]),
            "SDK legacy counter callback matches");
      continue;
    }
    auto ticks = std::stoull(row[7]);
    check(value.timestamp_ticks == ticks &&
              value.cpu_id == std::stoul(row[5]) &&
              value.thread_priority == std::stoul(row[6]),
          "SDK timestamp/cpu/priority callback matches");
    Key key{zone, id, thread, context};
    if (row[0] == "start") {
      check(value.kind == cpu::Kind::start, "SDK start kind");
      open[key].push_back(ticks);
    } else {
      check(value.kind == cpu::Kind::stop && !open[key].empty(),
            "SDK stop kind and pairing");
      auto begin = open[key].back();
      open[key].pop_back();
      bool found = false;
      for (const auto &span : index.intervals())
        if (span.start_source.zone.id == zone && span.event_id == id &&
            span.thread_id == thread && span.context_id == context &&
            span.begin_ticks == begin && span.end_ticks == ticks) {
          check(span.duration_ticks == ticks - begin,
                "SDK oracle interval duration");
          found = true;
          break;
        }
      check(found, "SDK oracle interval exists");
      ++expected_intervals;
    }
  }
  check(expected_events >= 15 && expected_intervals >= 7 && frequency,
        "real fixture exercises CPU scopes and clock");
  for (const auto &entry : cursor)
    check(entry.second == actual[entry.first].size(),
          "no unexpected fixture zone records");
  for (const auto &lane : open)
    check(lane.second.empty(), "SDK oracle scopes closed");
  check(session.header().timestamp_denominator == frequency &&
            session.header().timestamp_numerator == 1000000000ull,
        "SDK counter calibration is nanoseconds per QPC tick");
  check(index.intervals().size() == expected_intervals &&
            !index.statistics().open_scopes,
        "all real SDK intervals paired");
  for (const auto &span : index.intervals()) {
    check(bool(span.duration_nanoseconds),
          "SDK calibrated nanoseconds available");
    auto ns = static_cast<long double>(span.duration_ticks) * 1000000000ull /
              frequency;
    check(std::abs(*span.duration_nanoseconds - static_cast<double>(ns)) < 1e-5,
          "SDK nanoseconds no extra factor ten");
  }
  check(diagnostic(index, cpu::DiagnosticCode::legacy_value_truncation),
        "real SDK legacy value limitation exposed");
  for (const auto &d : index.diagnostics())
    check(d.code == cpu::DiagnosticCode::legacy_value_truncation ||
              d.code == cpu::DiagnosticCode::value_without_timestamp,
          "unexpected real SDK profile diagnostic");
  std::cout << "SDK profile: " << expected_events << " records, "
            << expected_intervals << " independently checked intervals\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 3)
      sdk(argv[1], argv[2]);
    else if (argc == 1) {
      pairing(ByteOrder::little);
      pairing(ByteOrder::big);
      compression(ByteOrder::little);
      compression(ByteOrder::big);
      crossing();
      corrupt();
      limits();
      incremental();
    } else
      throw std::runtime_error(
          "usage: profile_tests [capture.pxd2 expected.csv]");
    std::cout << "profile tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "profile tests failed: " << e.what() << '\n';
    return 1;
  }
}
