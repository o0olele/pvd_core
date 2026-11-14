#include "pvd/memory.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace m = pvd::memory;
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
struct Writer {
  Bytes bytes;
  ByteOrder order;
  std::size_t address_width;
  explicit Writer(ByteOrder o, std::size_t width = 8)
      : order(o), address_width(width) {}
  void u(std::uint64_t n, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          n >> (8 * (order == ByteOrder::little ? i : width - i - 1))));
  }
  void string(std::uint32_t handle, const char *text) {
    const std::string s(text);
    u(0x3ff1, 2);
    u(s.size() + 1, 4);
    bytes.insert(bytes.end(), s.begin(), s.end());
    u(0, 1);
    u(handle, 4);
  }
  void dictionary(const char *type = "WindowType") {
    string(1, type);
    string(2, "window.cpp");
  }
  void alloc(std::uint64_t address, std::uint32_t size) {
    u(address_width == 8 ? 0x3ff2 : 0x3fe2, 2);
    u(size, 4);
    u(1, 4);
    u(2, 4);
    u(42, 4);
    u(address, address_width);
  }
  void free(std::uint64_t address) {
    u(address_width == 8 ? 0x33 : 0x23, 2);
    u(address, address_width);
  }
};
ChannelPacket packet(Writer w, std::size_t index, m::Source source,
                     std::uint8_t width) {
  PropertyValue value;
  value.byte_order = w.order;
  value.pointer_width = width;
  value.item_count = static_cast<std::uint32_t>(w.bytes.size());
  value.bytes = std::make_shared<const Bytes>(std::move(w.bytes));
  return {index,    source.stream_id,      source.buffer_id,
          "events", "memory event stream", std::move(value)};
}
void synthetic(ByteOrder order, std::uint8_t width) {
  const m::Source a{1, 10}, b{2, 10}, c{1, 20};
  std::vector<ChannelPacket> packets;
  Writer first(order, width);
  first.dictionary();
  first.alloc(0x1000, 64);
  first.alloc(0x2000, 128);
  first.free(0x2000);
  packets.push_back(packet(std::move(first), 2, a, width));
  Writer second(order, width);
  second.dictionary("OtherStream");
  second.alloc(0x1000, 500);
  packets.push_back(packet(std::move(second), 3, b, width));
  Writer third(order, width);
  third.dictionary("OtherBuffer");
  third.alloc(0x1000, 700);
  packets.push_back(packet(std::move(third), 4, c, width));
  Writer activity(order, width);
  activity.free(0x1000);
  activity.alloc(0x1000, 70000);
  activity.alloc(0x1000, 80000);
  activity.free(0x9999);
  packets.push_back(packet(std::move(activity), 5, a, width));
  packets.push_back(packet(Writer(order, width), 7, a, width));
  Writer release(order, width);
  release.free(0x1000);
  packets.push_back(packet(std::move(release), 8, b, width));
  Writer full(order, width);
  full.u(4, 2);
  packets.push_back(packet(std::move(full), 9, c, width));
  Writer after_bad(order, width);
  after_bad.alloc(0x3000, 12);
  packets.push_back(packet(std::move(after_bad), 10, c, width));
  Writer broken(order, width);
  broken.alloc(0x4000, 40);
  broken.u(0x3ff2, 2);
  broken.u(10, 1);
  packets.push_back(packet(std::move(broken), 11, a, width));
  m::Index index(packets);
  check(index.indexed_event_count() == 12, "packet index coverage");
  auto early = index.window_statistics({0, 4}, m::GroupBy::type, a);
  check(early.complete && early.has_memory_channel &&
            early.packets_in_window == 1 &&
            early.activity.allocated_bytes == 192 &&
            early.activity.freed_bytes == 128 &&
            early.activity.allocation_count == 2 &&
            early.activity.free_count == 1 &&
            early.as_of_end.current_bytes == 64 &&
            early.as_of_end.peak_bytes == 192 &&
            early.activity.churn_bytes() == "320" &&
            early.activity.net_bytes() == "64",
        "same packet allocate/free order and peak retained");
  check(early.groups.size() == 1 && early.groups[0].type_name == "WindowType" &&
            early.groups[0].activity.allocated_bytes == 192,
        "source dictionary isolation");
  auto source_b = index.window_statistics({0, 6}, m::GroupBy::type, b);
  check(source_b.complete && source_b.as_of_end.current_bytes == 500 &&
            source_b.groups.at(0).type_name == "OtherStream",
        "same address/buffer different stream isolation");
  auto middle = index.window_statistics({4, 6}, m::GroupBy::type, a);
  check(!middle.complete && middle.activity.allocated_bytes == 150000 &&
            middle.activity.freed_bytes == 64 &&
            middle.activity.replaced_bytes == 70000 &&
            middle.activity.replacement_count == 1 &&
            middle.activity.free_count == 2 &&
            middle.activity.unknown_free_count == 1 &&
            middle.activity.allocation_count == 2 &&
            middle.activity.net_bytes() == "149936" &&
            middle.as_of_end.current_bytes == 80000 &&
            middle.as_of_end.freed_bytes == 192 &&
            middle.as_of_end.replaced_bytes == 70000,
        "replacement is separate from known and unknown frees");
  auto empty = index.window_statistics({6, 6}, m::GroupBy::type, a);
  check(empty.activity.allocation_count == 0 &&
            empty.activity.free_count == 0 &&
            empty.activity.churn_bytes() == "0" &&
            empty.as_of_end.current_bytes == 80000 && !empty.complete,
        "empty activity window retains incomplete cumulative baseline");
  auto no_channel = index.window_statistics({0, 2});
  check(!no_channel.has_memory_channel && no_channel.complete &&
            no_channel.activity.allocated_bytes == 0,
        "no observed memory channel is explicit");
  m::Index empty_channel(
      std::vector<ChannelPacket>{packet(Writer(order, width), 0, a, width)});
  const auto empty_packet = empty_channel.window_statistics({0, 1});
  check(empty_packet.complete && empty_packet.has_memory_channel &&
            empty_packet.packets_in_window == 1 &&
            empty_packet.activity.allocation_count == 0,
        "an observed empty channel differs from an absent memory channel");
  auto all = index.window_statistics({4, 10});
  check(all.packets_in_window == 5 && !all.complete &&
            all.activity.freed_bytes == 564 &&
            all.activity.allocated_bytes == 150700 &&
            all.activity.free_count == 3 &&
            all.as_of_end.current_bytes == 80700,
        "all sources global receipt-window accounting");
  auto partial = index.window_statistics({10, 12}, m::GroupBy::type, c);
  check(!partial.complete && partial.activity.allocation_count == 0 &&
            partial.packets_in_window == 1,
        "quarantined source cannot look complete in a later empty decoded "
        "window");
  auto good = index.window_statistics({10, 12}, m::GroupBy::type, b);
  check(good.complete && good.as_of_end.current_bytes == 0 &&
            good.activity.allocation_count == 0,
        "local source failure does not contaminate another source");
  auto broken_prefix = index.window_statistics({11, 12}, m::GroupBy::type, a);
  check(!broken_prefix.complete &&
            broken_prefix.activity.allocated_bytes == 40 &&
            broken_prefix.as_of_end.current_bytes == 80040,
        "malformed packet keeps only decoded inner prefix");
  const auto again = index.window_statistics({0, 4}, m::GroupBy::type, a);
  check(again.complete && again.as_of_end.current_bytes == 64 &&
            again.activity.churn_bytes() == "320",
        "reverse window query does not leak future free/failure/reuse");
  m::Options limits;
  limits.max_allocations = 1;
  m::Index limited(packets, limits);
  const auto capped = limited.window_statistics({0, 4}, m::GroupBy::type, a);
  check(!capped.complete && capped.has_memory_channel &&
            capped.activity.allocation_count == 1 &&
            capped.as_of_end.current_bytes == 64 &&
            capped.diagnostics.at(0).code == m::DiagnosticCode::limit_exceeded,
        "limit-exhausted activity is an explicit partial decoded prefix");
  limits = {};
  limits.max_packets = 1;
  m::Index packet_limited(packets, limits);
  const auto packet_cap = packet_limited.window_statistics({0, 12});
  check(!packet_cap.complete && packet_cap.packets_in_window == 1 &&
            packet_cap.activity.allocated_bytes == 192 &&
            packet_cap.diagnostics.at(0).code ==
                m::DiagnosticCode::limit_exceeded,
        "empty-packet receipt metadata has an explicit bounded global limit");
  rejects<std::out_of_range>([&] { index.window_statistics({7, 6}); },
                             "reversed interval rejected");
  rejects<std::out_of_range>([&] { index.window_statistics({0, 13}); },
                             "unindexed future rejected");
  rejects<std::invalid_argument>(
      [&] { index.window_statistics({0, 4}, static_cast<m::GroupBy>(99)); },
      "invalid grouping rejected");
}
struct SessionFixture {
  StreamHeader header;
  std::vector<pvd::Event> events;
  std::uint64_t stream = 1;
  std::map<std::uint64_t, std::map<std::string, std::uint32_t>> strings;
  void emit(Payload payload) {
    events.push_back(
        {stream, events.size(), events.size() * 17, std::move(payload)});
  }
  std::uint32_t str(const std::string &s) {
    auto &map = strings[stream];
    auto found = map.find(s);
    if (found != map.end())
      return found->second;
    auto h = static_cast<std::uint32_t>(map.size() + 1);
    map.emplace(s, h);
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *name) {
    auto ns = str("physx3"), n = str(name);
    return {ns, n};
  }
  void packet(Writer writer) {
    auto p = str("events");
    auto t = type("PvdU8");
    emit(SetPropertyValue{10, p, t,
                          static_cast<std::uint32_t>(writer.bytes.size()),
                          std::move(writer.bytes)});
  }
  void mark(bool begin, std::uint64_t scene, std::uint64_t tick) {
    auto h = str("frame");
    if (begin)
      emit(BeginSection{scene, h, tick});
    else
      emit(EndSection{scene, h, tick});
  }
  SessionFixture() {
    auto scene = type("PxScene"), memory = type("Memory");
    emit(CreateClass{scene});
    emit(CreateClass{memory});
    auto p = str("events"), semantic = str("memory event stream");
    auto t = type("PvdU8");
    emit(CreateProperty{memory, p, semantic, t, 2, {}});
    emit(CreateInstance{scene, 1});
    emit(CreateInstance{scene, 2});
    emit(CreateInstance{memory, 10});
    Writer first(ByteOrder::little);
    first.dictionary();
    first.alloc(100, 100);
    mark(true, 1, 1);
    packet(first);
    mark(false, 1, 10);
    mark(true, 2, 11);
    Writer second(ByteOrder::little);
    second.alloc(200, 200);
    packet(second);
    mark(false, 2, 20);
    mark(true, 1, 21);
    Writer third(ByteOrder::little);
    third.free(100);
    packet(third);
    mark(false, 1, 30);
    stream = 2;
    mark(true, 1, 31);
    Writer fourth(ByteOrder::little);
    fourth.alloc(300, 300);
    packet(fourth);
    mark(false, 1, 40);
  }
};
void boundaries() {
  SessionFixture fixture;
  Session session(fixture.header, fixture.events);
  const auto &frames = session.frames();
  check(frames.size() == 4, "multi scene/stream frames");
  auto first = m::native_frame_event_window(session, frames[0]);
  auto other_scene = m::native_frame_event_window(session, frames[1]);
  auto second = m::native_frame_event_window(session, frames[2]);
  auto other_stream = m::native_frame_event_window(session, frames[3]);
  check(first.begin_event_count == 0 && other_scene.begin_event_count == 0 &&
            other_stream.begin_event_count == 0 &&
            second.begin_event_count == frames[0].event_count &&
            second.end_event_count == frames[2].event_count,
        "previous native end is isolated by exact scene and stream");
  auto forged = frames[0];
  ++forged.end_timestamp;
  rejects<std::invalid_argument>(
      [&] { m::native_frame_event_window(session, forged); },
      "foreign/forged frame rejected");
  m::Index live;
  live.update(session, frames[0].event_count);
  check(live.window_statistics(first).as_of_end.current_bytes == 100,
        "live first receipt prefix");
  rejects<std::out_of_range>([&] { live.window_statistics(second); },
                             "not yet indexed later frame is not zero");
  live.update(session);
  live.update(session, frames[0].event_count);
  auto activity = live.window_statistics(second);
  check(activity.activity.allocated_bytes == 200 &&
            activity.activity.freed_bytes == 100 &&
            activity.as_of_end.current_bytes == 200,
        "window includes other scene packets without false scene attribution");
  check(live.window_statistics(first).as_of_end.current_bytes == 100,
        "reverse seek after live update");
  auto late =
      live.window_statistics({frames[2].event_count, session.event_count()});
  check(late.activity.allocated_bytes == 300 && late.activity.freed_bytes == 0,
        "late packet remains in actual receipt window rather than "
        "back-attributed");
}
std::vector<std::string> split(const std::string &line) {
  std::vector<std::string> r;
  std::istringstream in(line);
  std::string v;
  while (std::getline(in, v, ','))
    r.push_back(v);
  return r;
}
void real(const char *capture, const char *summary_path) {
  auto session = Session::load_file(capture);
  m::Index index(session);
  std::map<unsigned, std::size_t> phases;
  for (const auto &marker : session.sections())
    if (marker.name.rfind("memory_validation_", 0) == 0)
      phases[static_cast<unsigned>(std::stoul(marker.name.substr(18)))] =
          marker.event_count;
  check(phases.size() == 5, "five SDK phase markers");
  std::ifstream input(summary_path);
  check(bool(input), "SDK summary oracle input");
  std::string line;
  std::getline(input, line);
  std::map<unsigned, std::vector<std::uint64_t>> expected;
  while (std::getline(input, line)) {
    auto fields = split(line);
    check(fields.size() == 7, "SDK summary columns");
    auto &values = expected[static_cast<unsigned>(std::stoul(fields[0]))];
    for (std::size_t i = 1; i < fields.size(); ++i)
      values.push_back(std::stoull(fields[i]));
  }
  check(expected.size() == 5, "five SDK summary phases");
  for (bool reverse : {false, true})
    for (unsigned i = 0; i < 5; ++i) {
      const unsigned phase = reverse ? 4 - i : i;
      const auto begin = phase ? phases.at(phase - 1) : 0;
      auto result = index.window_statistics({begin, phases.at(phase)},
                                            m::GroupBy::type_file_line);
      m::ActivityStats flow;
      std::uint64_t current = 0, allocated = 0, freed = 0;
      std::size_t allocations = 0, frees = 0;
      for (const auto &group : result.groups)
        if (group.type_name &&
            group.type_name->rfind("MemoryFixture.", 0) == 0) {
          current += group.as_of_end.current_bytes;
          allocated += group.as_of_end.allocated_bytes;
          freed += group.as_of_end.freed_bytes;
          allocations += group.as_of_end.allocation_count;
          frees += group.as_of_end.free_count;
          flow.allocated_bytes += group.activity.allocated_bytes;
          flow.freed_bytes += group.activity.freed_bytes;
          flow.allocation_count += group.activity.allocation_count;
          flow.free_count += group.activity.free_count;
        }
      const auto &end = expected.at(phase);
      std::vector<std::uint64_t> previous(6);
      if (phase)
        previous = expected.at(phase - 1);
      check(current == end[0] && allocated == end[2] && freed == end[3] &&
                allocations == end[4] && frees == end[5],
            "SDK allocator cumulative oracle matches window endpoint");
      check(flow.allocated_bytes == end[2] - previous[2] &&
                flow.freed_bytes == end[3] - previous[3] &&
                flow.allocation_count == end[4] - previous[4] &&
                flow.free_count == end[5] - previous[5],
            "SDK callback phase allocation/free delta oracle");
      check(result.has_memory_channel && !result.complete,
            "real SDK pre-listener unknown frees stay explicit");
      for (const auto &diagnostic : result.diagnostics)
        check(diagnostic.code == m::DiagnosticCode::unknown_free,
              "unexpected real SDK parse diagnostic");
    }
  std::cout << "Real SDK five-phase receipt windows forward/reverse passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    for (auto order : {ByteOrder::little, ByteOrder::big})
      for (auto width : {std::uint8_t(4), std::uint8_t(8)})
        synthetic(order, width);
    boundaries();
    m::ActivityStats exact;
    exact.allocated_bytes = exact.freed_bytes = UINT64_MAX;
    check(exact.churn_bytes() == "36893488147419103230" &&
              exact.net_bytes() == "0",
          "exact activity arithmetic above u64");
    exact.allocated_bytes = 0;
    check(exact.net_bytes() == "-18446744073709551615",
          "signed net never passes through double or int64");
    if (argc == 3)
      real(argv[1], argv[2]);
    else
      check(argc == 1, "usage: memory_window_tests [capture.pxd2 summary.csv]");
    std::cout << "Memory receipt-window tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
