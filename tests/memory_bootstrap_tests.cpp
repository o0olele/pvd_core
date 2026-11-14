#include <algorithm>
#include <iostream>
#include <map>
#include <pvd/memory_bootstrap.hpp>
#include <pvd/writer.hpp>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace mem = pvd::memory;
namespace writing = pvd::writing;
void check(bool b, const std::string &message) {
  if (!b)
    throw std::runtime_error(message);
}
template <class F> void refuses(F fn, const char *message) {
  try {
    fn();
  } catch (const mem::BootstrapError &) {
    return;
  }
  throw std::runtime_error(message);
}
struct Inner {
  ByteOrder order;
  Bytes bytes;
  explicit Inner(ByteOrder o) : order(o) {}
  void u(std::uint64_t n, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          n >> ((order == ByteOrder::little ? i : width - i - 1) * 8)));
  }
  void string(std::uint32_t handle, const std::string &text) {
    u(0x3ff1, 2);
    u(text.size() + 1, 4);
    bytes.insert(bytes.end(), text.begin(), text.end());
    u(0, 1);
    u(handle, 4);
  }
  void alloc(std::uint64_t address, std::uint32_t size, std::uint32_t type = 1,
             std::uint32_t file = 2) {
    u(0x3ff2, 2);
    u(size, 4);
    u(type, 4);
    u(file, 4);
    u(42, 4);
    u(address, 8);
  }
  void free(std::uint64_t address) {
    u(0x33, 2);
    u(address, 8);
  }
};
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  std::size_t begin = 0, end = 0;
  void emit(Payload p) {
    events.push_back(
        {1, events.size() * 100, events.size() * 10, std::move(p)});
  }
  std::uint32_t str(const std::string &name) {
    auto found = strings.find(name);
    if (found != strings.end())
      return found->second;
    auto h = static_cast<std::uint32_t>(strings.size() + 1);
    strings[name] = h;
    emit(StringHandleEvent{name, h});
    return h;
  }
  WireName type(const std::string &name) {
    auto ns = str("physx3"), h = str(name);
    return {ns, h};
  }
  void packet(std::uint64_t buffer, const Bytes &bytes,
              const char *prop = "events") {
    auto name = str(prop);
    auto dt = type("PvdU8");
    emit(SetPropertyValue{buffer, name, dt,
                          static_cast<std::uint32_t>(bytes.size()), bytes});
  }
  explicit Fixture(ByteOrder order = ByteOrder::little, unsigned width = 8) {
    header.byte_order = order;
    header.stream_flags = width == 8 ? 1 : 0;
    auto cls = type("MemoryBuffer");
    emit(CreateClass{cls});
    auto dt = type("PvdU8");
    auto prop = str("events"), sem = str("memory event stream");
    emit(CreateProperty{cls, prop, sem, dt, 2, {}});
    emit(CreateInstance{cls, 10});
    emit(CreateInstance{cls, 20});
    Inner first(order);
    first.string(1, "OldType");
    first.string(2, "old.cpp");
    first.string(3, "Seen but unused \xe5\x90\x8d");
    first.alloc(0x1000, 5);
    first.free(0x1000);
    first.alloc(0x1000, 10);
    first.alloc(0x2000, 20);
    first.free(0x2000);
    packet(10, first.bytes);
    Inner second(order);
    second.string(1, "OtherType");
    second.string(2, "");
    second.alloc(0x1000, 7);
    packet(20, second.bytes);
    begin = events.size();
    Inner future(order);
    future.string(4, "FUTURE_TYPE");
    future.free(0x1000);
    future.alloc(0x3000, 30, 4);
    packet(10, future.bytes);
    Inner reuse(order);
    reuse.free(0x1000);
    reuse.alloc(0x1000, 11);
    packet(20, reuse.bytes);
    end = events.size();
  }
  Session session() const { return Session(header, events); }
};
ChannelPacket packet(const mem::SourceBootstrap &b, ByteOrder order,
                     std::uint8_t width, std::size_t event_index) {
  PropertyValue v;
  v.incoming_type = {"physx3", "PvdU8"};
  v.byte_order = order;
  v.pointer_width = width;
  v.bytes = std::make_shared<const Bytes>(b.bytes);
  v.item_count = b.bytes.size();
  return {event_index,     b.source.stream_id,    b.source.buffer_id,
          b.property_name, "memory event stream", std::move(v)};
}
using ActiveKey = std::tuple<mem::Source, std::uint64_t>;
using ActiveValue = std::tuple<std::uint64_t, std::optional<std::string>,
                               std::optional<std::string>, std::uint32_t>;
std::map<ActiveKey, ActiveValue> active(const mem::Snapshot &snapshot) {
  std::map<ActiveKey, ActiveValue> rows;
  for (const auto &a : snapshot.active)
    rows.emplace(ActiveKey{a.source, a.address},
                 ActiveValue{a.size, a.type_name, a.file, a.line});
  return rows;
}
writing::CropOptions providers(const mem::Bootstrap &bootstrap) {
  writing::CropOptions options;
  options.allow_partial_analysis = bootstrap.partial;
  for (const auto &source : bootstrap.sources) {
    writing::ChannelBootstrap b;
    b.stream_id = source.source.stream_id;
    b.instance_id = source.source.buffer_id;
    b.instance_generation = source.buffer_generation;
    b.property_name = source.property_name;
    b.semantic = "memory event stream";
    b.bytes = source.bytes;
    b.partial = bootstrap.partial;
    if (bootstrap.partial)
      for (const auto &d : bootstrap.diagnostics)
        b.warnings.push_back("Partial original memory baseline: " + d.message);
    options.channel_bootstrap.push_back(std::move(b));
  }
  return options;
}
void compare_stats(const mem::Stats &initial, const mem::Stats &original_end,
                   const mem::Stats &cropped) {
  check(cropped.current_bytes == original_end.current_bytes &&
            cropped.current_allocations == original_end.current_allocations,
        "cropped current state differs");
  check(cropped.allocated_bytes == initial.current_bytes +
                                       original_end.allocated_bytes -
                                       initial.allocated_bytes,
        "cropped allocated-byte baseline differs");
  check(cropped.freed_bytes == original_end.freed_bytes - initial.freed_bytes &&
            cropped.replaced_bytes ==
                original_end.replaced_bytes - initial.replaced_bytes,
        "cropped observed release accounting differs");
  check(cropped.allocation_count == initial.current_allocations +
                                        original_end.allocation_count -
                                        initial.allocation_count,
        "cropped allocation-count baseline differs");
  check(cropped.free_count == original_end.free_count - initial.free_count &&
            cropped.unknown_free_count ==
                original_end.unknown_free_count - initial.unknown_free_count,
        "cropped free counters differ");
}
void canonical_and_continuation() {
  for (auto order : {ByteOrder::little, ByteOrder::big})
    for (unsigned width : {4u, 8u}) {
      Fixture f(order, width);
      auto s = f.session();
      mem::Index original(s);
      auto bootstrap = mem::make_bootstrap(s, f.begin);
      check(bootstrap.sources.size() == 2 && !bootstrap.partial &&
                bootstrap.diagnostics.empty(),
            "valid prefix bootstrap incomplete");
      check(bootstrap.sources[0].dictionary_entries == 3 &&
                bootstrap.sources[0].active_allocations == 1 &&
                bootstrap.sources[1].dictionary_entries == 2,
            "dictionary/active count mismatch");
      std::vector<ChannelPacket> packets;
      for (const auto &b : bootstrap.sources)
        packets.push_back(
            packet(b, order, static_cast<std::uint8_t>(width), f.begin - 1));
      mem::Index baseline(packets);
      auto state = baseline.snapshot_at_event(f.begin);
      check(active(state) == active(original.snapshot_at_event(f.begin)),
            "bootstrap did not restore source-isolated active state");
      check(state.totals.current_bytes == 17 &&
                state.totals.allocated_bytes == 17 &&
                state.totals.freed_bytes == 0 &&
                state.totals.peak_bytes == 17 &&
                state.totals.allocation_count == 2,
            "bootstrap baseline statistics wrong");
      check(baseline.events().size() == 7 && baseline.allocations().size() == 2,
            "retired allocations/frees leaked into canonical bootstrap");
      check(baseline.allocations()[0].generation == 0 &&
                original.snapshot_at_event(f.begin).active[0].generation == 1,
            "allocation generations not explicitly rebased");
      for (const auto &e : baseline.events())
        check(e.string_value != "FUTURE_TYPE", "future dictionary leaked");
      std::size_t bytes = 0;
      for (const auto &b : bootstrap.sources)
        bytes += b.bytes.size();
      check(bytes == bootstrap.total_bytes, "bootstrap byte count wrong");
      for (const auto &p : s.channels())
        if (p.event_index >= f.begin && p.event_index < f.end)
          packets.push_back(p);
      mem::Index continued(packets);
      auto end = continued.snapshot_at_event(f.end);
      check(end.complete &&
                active(end) == active(original.snapshot_at_event(f.end)),
            "dictionary/active state lost across selected packets");
      check(end.totals.current_bytes == 41 &&
                end.totals.allocated_bytes == 58 &&
                end.totals.freed_bytes == 17 && end.totals.peak_bytes == 41,
            "continued crop cumulative/peak semantics wrong");
      auto crop = writing::crop_events(s, f.begin, f.end, providers(bootstrap));
      Session roundtrip(crop.header, crop.events);
      mem::Index actual(roundtrip);
      auto decoded = actual.snapshot_at_event(roundtrip.event_count());
      check(decoded.complete && active(decoded) == active(end),
            "writer bootstrap selected stream IDs/handles wrong");
      compare_stats(original.snapshot_at_event(f.begin).totals,
                    original.snapshot_at_event(f.end).totals, decoded.totals);
      std::ostringstream raw;
      writing::write_session(raw, roundtrip);
      auto text = raw.str();
      Bytes encoded(text.begin(), text.end());
      auto reparsed = Session::parse(encoded);
      mem::Index reparsed_memory(reparsed);
      check(active(reparsed_memory.snapshot_at_event(reparsed.event_count())) ==
                active(end),
            "writer bytes failed core reparse");
    }
}
void rejects_and_boundaries() {
  Fixture f;
  auto s = f.session();
  auto expected = mem::make_bootstrap(s, f.begin);
  mem::BootstrapOptions o;
  o.max_total_bytes = expected.total_bytes;
  check(mem::make_bootstrap(s, f.begin, o).total_bytes == expected.total_bytes,
        "exact output budget failed");
  o.max_total_bytes = expected.total_bytes - 1;
  refuses([&] { mem::make_bootstrap(s, f.begin, o); },
          "output byte bound ignored");
  o = {};
  o.max_input_bytes = 0;
  refuses([&] { mem::make_bootstrap(s, f.begin, o); },
          "input byte bound ignored");
  o = {};
  o.max_sources = 1;
  refuses([&] { mem::make_bootstrap(s, f.begin, o); }, "source bound ignored");
  o = {};
  o.max_dictionary_entries = 4;
  refuses([&] { mem::make_bootstrap(s, f.begin, o); },
          "dictionary bound ignored");
  o = {};
  o.max_active_allocations = 1;
  refuses([&] { mem::make_bootstrap(s, f.begin, o); },
          "active allocation bound ignored");
  o = {};
  o.decode_limits.max_memory_events = 1;
  refuses([&] { mem::make_bootstrap(s, f.begin, o); }, "decode bound ignored");
  refuses([&] { mem::make_bootstrap(s, s.event_count() + 1); },
          "out-of-range prefix accepted");
  o = {};
  o.max_sources = 0;
  o.max_total_bytes = 0;
  o.max_input_bytes = 0;
  check(mem::make_bootstrap(s, 0, o).sources.empty(),
        "empty prefix is not empty");
  // Later corruption and dictionary text must not contaminate an earlier
  // prefix.
  Inner corrupt(ByteOrder::little);
  corrupt.u(5, 2);
  f.packet(10, corrupt.bytes);
  auto future = f.session();
  check(mem::make_bootstrap(future, f.begin).total_bytes ==
            expected.total_bytes,
        "future corrupt packet leaked into bootstrap");
  refuses([&] { mem::make_bootstrap(future, future.event_count()); },
          "malformed prefix accepted");
  for (unsigned mode = 0; mode != 5; ++mode) {
    Fixture damaged;
    Inner bad(ByteOrder::little);
    if (mode == 0)
      bad.alloc(123, 1, 999);
    else if (mode == 1)
      bad.string(1, "CONFLICT");
    else if (mode == 2)
      bad.alloc(0x3000, 50);
    else if (mode == 3)
      bad.u(4, 2);
    else
      bad.u(0x4001, 2);
    damaged.packet(10, bad.bytes);
    auto input = damaged.session();
    mem::BootstrapOptions partial;
    partial.unknown_frees = mem::UnknownFreePolicy::allow_partial;
    refuses([&] { mem::make_bootstrap(input, input.event_count(), partial); },
            "non-unknown-free diagnostic accepted as partial");
  }
  Fixture unknown;
  Inner bad(ByteOrder::little);
  bad.free(0xdead);
  unknown.packet(10, bad.bytes);
  auto u = unknown.session();
  refuses([&] { mem::make_bootstrap(u, u.event_count()); },
          "unknown free silently lost");
  o = {};
  o.unknown_frees = mem::UnknownFreePolicy::allow_partial;
  auto partial = mem::make_bootstrap(u, u.event_count(), o);
  check(partial.partial && partial.diagnostics.size() == 1 &&
            partial.diagnostics[0].code == mem::DiagnosticCode::unknown_free,
        "explicit partial provenance lost");
  Fixture retired_partial;
  Inner unknown_retired(ByteOrder::little);
  unknown_retired.free(0xbeef);
  retired_partial.packet(10, unknown_retired.bytes);
  retired_partial.emit(DestroyInstance{10});
  auto retired = retired_partial.session();
  auto live_only = mem::make_bootstrap(retired, retired.event_count());
  check(!live_only.partial && live_only.diagnostics.empty() &&
            live_only.sources.size() == 1 &&
            live_only.sources[0].source.buffer_id == 20,
        "retired source diagnostics contaminated live source baseline");
  Fixture destroyed;
  destroyed.emit(DestroyInstance{20});
  auto dead = destroyed.session();
  auto only = mem::make_bootstrap(dead, dead.event_count());
  check(only.sources.size() == 1 && only.sources[0].source.buffer_id == 10,
        "destroyed buffer resurrected");
  destroyed.emit(CreateInstance{destroyed.type("MemoryBuffer"), 20});
  auto reused = destroyed.session();
  refuses([&] { mem::make_bootstrap(reused, reused.event_count()); },
          "reused buffer identity accepted");
}
void real_sdk(const char *path) {
  auto session = Session::load_file(path);
  std::map<unsigned, std::size_t> phases;
  for (const auto &section : session.sections())
    if (section.name.rfind("memory_validation_", 0) == 0)
      phases[std::stoul(section.name.substr(18))] = section.event_count;
  check(phases.size() == 5, "real memory fixture needs five phases");
  mem::Index original(session);
  mem::BootstrapOptions partial;
  partial.unknown_frees = mem::UnknownFreePolicy::allow_partial;
  for (unsigned begin_phase = 0; begin_phase != 4; ++begin_phase) {
    const auto begin = phases.at(begin_phase), end = phases.at(4);
    auto bootstrap = mem::make_bootstrap(session, begin, partial);
    check(bootstrap.partial && !bootstrap.diagnostics.empty(),
          "real SDK partial provenance missing");
    refuses([&] { mem::make_bootstrap(session, begin); },
            "strict real SDK partial prefix not rejected");
    auto cropped =
        writing::crop_events(session, begin, end, providers(bootstrap));
    check(!cropped.warnings.empty(),
          "writer dropped partial baseline warnings");
    Session decoded(cropped.header, cropped.events);
    mem::Index actual(decoded);
    auto result = actual.snapshot_at_event(decoded.event_count()),
         expected = original.snapshot_at_event(end),
         initial = original.snapshot_at_event(begin);
    check(active(result) == active(expected),
          "real mid-crop active state differs from original");
    compare_stats(initial.totals, expected.totals, result.totals);
    bool warning = false;
    for (const auto &event : decoded.events())
      if (const auto *e = std::get_if<ErrorMessage>(&event.payload))
        if (e->message.find("Partial original memory baseline") !=
            std::string::npos)
          warning = true;
    check(warning, "partial warning not persisted in crop wire events");
    std::cout << "real memory crop phase " << begin_phase
              << " -> 4: " << bootstrap.total_bytes << " bootstrap bytes / "
              << bootstrap.sources.size() << " sources / "
              << result.active.size() << " final active allocations\n";
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    canonical_and_continuation();
    rejects_and_boundaries();
    if (argc > 1)
      real_sdk(argv[1]);
    std::cout << "memory bootstrap tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
