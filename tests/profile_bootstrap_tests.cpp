#include "pvd/profile_bootstrap.hpp"
#include "pvd/writer.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace cpu = pvd::profile;
namespace io = pvd::writing;
void check(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
template <class F> void rejects(F &&f) {
  try {
    f();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("expected bootstrap rejection");
}
struct Inner {
  ByteOrder order;
  Bytes bytes;
  explicit Inner(ByteOrder o) : order(o) {}
  void u(std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          value >> ((order == ByteOrder::little ? i : width - 1 - i) * 8)));
  }
  void full(unsigned tag, std::uint16_t id, std::uint32_t thread,
            std::uint64_t context, std::uint64_t ticks) {
    u(tag, 1);
    u(15, 1);
    u(id, 2);
    u(thread, 4);
    u(context, 8);
    u(4, 1);
    u(3, 1);
    u(ticks, 8);
  }
  void relative(unsigned tag, std::uint16_t id, std::uint64_t delta) {
    u(tag, 1);
    u(0, 1);
    u(id, 2);
    u(delta, 1);
  }
  void value(std::uint16_t id, std::uint64_t raw) {
    u(5, 1);
    u(15, 1);
    u(id, 2);
    u(raw, 8);
    u(44, 8);
    u(55, 4);
  }
  void gpu() {
    u(6, 1);
    u(0xff, 1);
    u(0, 2);
    u(1, 8);
    u(0, 4);
    u(0, 4);
    u(0, 4);
    u(7, 4);
  }
};
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::uint64_t, std::map<std::string, std::uint32_t>> handles;
  explicit Fixture(ByteOrder order = ByteOrder::little, bool ptr64 = true) {
    header.byte_order = order;
    header.stream_flags = ptr64 ? 1 : 0;
    header.timestamp_numerator = 100;
    header.timestamp_denominator = 3;
  }
  template <class T> void emit(T value, std::uint64_t stream = 1) {
    events.push_back(
        {stream, events.size() * 19, 0, Payload{std::move(value)}});
  }
  std::uint32_t str(const std::string &s, std::uint64_t stream = 1) {
    if (s.empty())
      return 0;
    auto &table = handles[stream];
    auto f = table.find(s);
    if (f != table.end())
      return f->second;
    auto h = static_cast<std::uint32_t>(table.size() + 1);
    table.emplace(s, h);
    emit(StringHandleEvent{s, h}, stream);
    return h;
  }
  WireName name(const char *ns, const char *n, std::uint64_t stream = 1) {
    auto a = str(ns, stream), b = str(n, stream);
    return {a, b};
  }
  void setup() {
    auto cls = name("_debugger_", "PxProfileZone");
    emit(CreateClass{cls});
    auto prop = str("packets"), semantic = str("profile event stream");
    auto type = name("physx3", "PvdU8");
    emit(CreateProperty{cls, prop, semantic, type, 2, {}});
    announce();
  }
  void announce() {
    emit(AddProfileZone{10, "CPU Zone"});
    emit(CreateInstance{name("_debugger_", "PxProfileZone"), 10});
    for (unsigned i = 1; i <= 6; ++i)
      emit(AddProfileZoneEvent{10, "Scope " + std::to_string(i),
                               static_cast<std::uint16_t>(i), true});
  }
  void data(const Inner &inner, std::uint64_t stream = 1) {
    auto property = str("packets", stream);
    auto type = name("physx3", "PvdU8", stream);
    emit(SetPropertyValue{10, property, type,
                          static_cast<std::uint32_t>(inner.bytes.size()),
                          inner.bytes},
         stream);
  }
};
io::CropOptions adapt(const cpu::Bootstrap &bootstrap) {
  io::CropOptions options;
  for (const auto &source : bootstrap.sources)
    options.channel_bootstrap.push_back({source.stream_id,
                                         source.zone.id,
                                         source.buffer_generation,
                                         source.property_name,
                                         "profile event stream",
                                         source.bytes,
                                         false,
                                         {}});
  return options;
}
void spans(const cpu::Index &original, const cpu::Index &selected,
           std::size_t begin, std::size_t end) {
  std::size_t expected = 0;
  for (const auto &interval : original.intervals())
    if (interval.stop_source.event_index >= begin &&
        interval.stop_source.event_index < end) {
      ++expected;
      bool found = false;
      for (const auto &other : selected.intervals())
        if (interval.event_id == other.event_id &&
            interval.thread_id == other.thread_id &&
            interval.context_id == other.context_id &&
            interval.begin_ticks == other.begin_ticks &&
            interval.end_ticks == other.end_ticks &&
            interval.start_source.zone == other.start_source.zone) {
          check(interval.duration_ticks == other.duration_ticks &&
                    interval.duration_nanoseconds ==
                        other.duration_nanoseconds &&
                    interval.cpu_id == other.cpu_id &&
                    interval.thread_priority == other.thread_priority,
                "continued interval duration/clock/context");
          check(interval.start_source.stream_id == other.start_source.stream_id,
                "continued original start source stream");
          found = true;
          break;
        }
      check(found, "continued interval found");
    }
  check(selected.intervals().size() == expected,
        "closed pre-crop scopes are excluded");
}
void synthetic(ByteOrder order, bool ptr64) {
  Fixture f(order, ptr64);
  f.setup();
  const auto base = 0xfedcba9876543200ull;
  Inner first(order);
  first.full(1, 1, 7, 9, base);
  first.relative(3, 1, 1);
  first.relative(3, 2, 2);
  first.full(1, 6, 8, 10, base + 5);
  first.relative(4, 6, 10);
  first.value(3, 70000);
  first.value(3, 80000);
  first.value(4, UINT64_MAX);
  f.data(first);
  const auto begin = f.events.size();
  Inner last(order);
  last.full(2, 2, 7, 9, base + 40);
  last.relative(4, 1, 1);
  last.relative(4, 1, 2);
  last.full(1, 5, 8, 10, base + 50);
  last.relative(4, 5, 10);
  f.data(last);
  Session source(f.header, f.events);
  auto bootstrap = cpu::make_bootstrap(source, begin);
  check(bootstrap.sources.size() == 1 &&
            bootstrap.sources[0].open_scopes == 3 &&
            bootstrap.sources[0].counters == 2 &&
            bootstrap.total_bytes == 3 * 26 + 2 * 24,
        "bootstrap only outstanding scopes and latest counters");
  check(bootstrap.sources[0].property_name == "packets" &&
            bootstrap.sources[0].records.size() == 5,
        "source property and original offset map");
  auto options = adapt(bootstrap);
  auto crop = io::crop_events(source, begin, source.event_count(), options);
  Session selected(crop.header, crop.events);
  cpu::Index a(source), b(selected);
  spans(a, b, begin, source.event_count());
  check(!b.statistics().open_scopes, "all continued scopes close");
  std::size_t counters = 0;
  for (const auto &event : b.events())
    if (event.kind == cpu::Kind::value) {
      ++counters;
      check(event.value == 80000 || event.value == -1,
            "only latest raw counter values retained");
    }
  check(counters == 2, "counter baseline count");
  for (const auto &map : bootstrap.sources[0].records) {
    const auto &packet = selected.channels()[0];
    check(packet.stream_id == 1 && map.original.stream_id == 1 &&
              map.original.zone == bootstrap.sources[0].zone,
          "canonical provenance source identity");
    check(map.payload_offset < packet.value.raw_bytes().size(),
          "canonical record offset bounds");
  }
  std::ostringstream encoded;
  io::write_session(encoded, selected);
  const auto data = encoded.str();
  auto loaded = Session::parse(Bytes(data.begin(), data.end()));
  cpu::Index c(loaded);
  spans(a, c, begin, source.event_count());
  auto complete = cpu::make_bootstrap(source, source.event_count());
  check(complete.sources[0].open_scopes == 0 &&
            complete.sources[0].counters == 2,
        "completed scopes absent from EOF baseline");
  cpu::BootstrapOptions small;
  small.max_open_scopes = 2;
  rejects([&] { cpu::make_bootstrap(source, begin, small); });
  small = {};
  small.max_counters = 1;
  rejects([&] { cpu::make_bootstrap(source, begin, small); });
  small = {};
  small.max_total_bytes = 1;
  rejects([&] { cpu::make_bootstrap(source, begin, small); });
  small = {};
  small.max_input_bytes = 1;
  rejects([&] { cpu::make_bootstrap(source, begin, small); });
  small = {};
  small.max_sources = 0;
  rejects([&] { cpu::make_bootstrap(source, begin, small); });
  small = {};
  small.decode_limits.max_events = 1;
  rejects([&] { cpu::make_bootstrap(source, begin, small); });
  rejects([&] { cpu::make_bootstrap(source, source.event_count() + 1); });
}
void corruption() {
  for (unsigned kind = 0; kind < 4; ++kind) {
    Fixture f;
    f.setup();
    Inner packet(f.header.byte_order);
    if (kind == 0)
      packet.bytes = {255, 0, 0, 0};
    else if (kind == 1) {
      packet.full(1, 1, 7, 9, 100);
      packet.bytes.pop_back();
    } else if (kind == 2)
      packet.gpu();
    else
      packet.full(2, 1, 7, 9, 100);
    f.data(packet);
    Session source(f.header, f.events);
    rejects([&] { cpu::make_bootstrap(source, source.event_count()); });
  }
  Fixture streams;
  streams.setup();
  Inner a(streams.header.byte_order);
  a.full(1, 1, 7, 9, 100);
  streams.data(a);
  Inner b(streams.header.byte_order);
  b.full(2, 1, 7, 9, 200);
  streams.data(b, 2);
  Session multi(streams.header, streams.events);
  rejects([&] { cpu::make_bootstrap(multi, multi.event_count()); });
}
void reuse() {
  Fixture f;
  f.setup();
  Inner old(f.header.byte_order);
  old.full(1, 1, 7, 9, 100);
  old.relative(4, 1, 10);
  f.data(old);
  f.emit(DestroyInstance{10});
  f.announce();
  Inner start(f.header.byte_order);
  start.full(1, 2, 7, 9, 300);
  f.data(start);
  const auto begin = f.events.size();
  Inner stop(f.header.byte_order);
  stop.full(2, 2, 7, 9, 400);
  f.data(stop);
  Session original(f.header, f.events);
  auto bootstrap = cpu::make_bootstrap(original, begin);
  check(bootstrap.sources.size() == 1 &&
            bootstrap.sources[0].buffer_generation == 1 &&
            bootstrap.sources[0].zone.generation == 1 &&
            bootstrap.sources[0].open_scopes == 1,
        "profile zone reuse bootstrap generation");
  auto crop = io::crop_events(original, begin, original.event_count(),
                              adapt(bootstrap));
  Session selected(crop.header, crop.events);
  cpu::Index a(original), b(selected);
  spans(a, b, begin, original.event_count());
  check(b.diagnostics().empty(), "reused zone names metadata retained");
  auto events = f.events;
  events.push_back({1, 0, 0, DestroyInstance{10}});
  Session dead(f.header, events);
  check(cpu::make_bootstrap(dead, dead.event_count()).sources.empty(),
        "destroyed profile sources do not require recreation");
}
void sdk(const char *file) {
  auto source = Session::load_file(file);
  cpu::Index original(source);
  std::size_t checked = 0, cross = 0;
  for (const auto &packet : source.channels()) {
    if (!cpu::is_profile_packet(packet))
      continue;
    const auto begin = packet.event_index + 1;
    if (begin >= source.event_count())
      continue;
    auto bootstrap = cpu::make_bootstrap(source, begin);
    auto crop =
        io::crop_events(source, begin, source.event_count(), adapt(bootstrap));
    Session selected(crop.header, crop.events);
    cpu::Index index(selected);
    spans(original, index, begin, source.event_count());
    for (const auto &src : bootstrap.sources)
      cross += src.open_scopes;
    for (const auto &diagnostic : index.diagnostics())
      check(diagnostic.code == cpu::DiagnosticCode::legacy_value_truncation ||
                diagnostic.code == cpu::DiagnosticCode::value_without_timestamp,
            "real SDK continuation diagnostics");
    ++checked;
  }
  check(checked >= 3 && cross >= 1,
        "real SDK cuts include start/flush/stop crossing");
  std::cout << "SDK profile bootstrap: " << checked << " cut points, " << cross
            << " retained open scopes\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 1) {
      synthetic(ByteOrder::little, true);
      synthetic(ByteOrder::big, true);
      synthetic(ByteOrder::little, false);
      synthetic(ByteOrder::big, false);
      corruption();
      reuse();
    } else
      for (int i = 1; i < argc; ++i)
        sdk(argv[i]);
    std::cout << "profile bootstrap tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "profile bootstrap tests failed: " << e.what() << '\n';
    return 1;
  }
}
