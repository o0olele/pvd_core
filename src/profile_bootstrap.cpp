#include "pvd/profile_bootstrap.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace pvd::profile {
namespace {
void add(std::size_t &value, std::size_t amount, std::size_t limit,
         const char *name) {
  if (value > limit || amount > limit - value)
    throw BootstrapError(std::string(name) + " budget exceeded");
  value += amount;
}
using Key = std::pair<std::uint64_t, std::uint64_t>; // stream, buffer
using Lane = std::tuple<ZoneKey, std::uint32_t, std::uint64_t, std::uint16_t>;
struct Build {
  std::string property;
  bool ambiguous_property = false;
  const ObjectLifetime *lifetime = nullptr;
  ZoneKey zone;
  std::vector<const Event *> starts;
  std::map<Lane, const Event *> counters;
};
struct Writer {
  ByteOrder order;
  Bytes bytes;
  void u(std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          value >> ((order == ByteOrder::little ? i : width - 1 - i) * 8)));
  }
  void header(unsigned tag, std::uint16_t id) {
    u(tag, 1);
    u(15, 1);
    u(id, 2);
  }
  void start(const Event &e) {
    header(1, e.event_id);
    u(e.thread_id, 4);
    u(e.context_id, 8);
    u(e.thread_priority, 1);
    u(e.cpu_id, 1);
    u(*e.timestamp_ticks, 8);
  }
  void value(const Event &e) {
    header(5, e.event_id);
    u(static_cast<std::uint64_t>(*e.value), 8);
    u(e.context_id, 8);
    u(e.thread_id, 4);
  }
};
} // namespace
Bootstrap make_bootstrap(const Session &session, std::size_t prefix,
                         const BootstrapOptions &options) {
  if (prefix > session.event_count())
    throw BootstrapError(
        "profile bootstrap prefix exceeds Session event count");
  Bootstrap result;
  result.event_count = prefix;
  std::map<Key, Build> builds;
  std::size_t input_bytes = 0;
  for (const auto &packet : session.channels()) {
    if (packet.event_index >= prefix)
      break;
    if (!is_profile_packet(packet))
      continue;
    add(input_bytes, packet.value.raw_bytes().size(), options.max_input_bytes,
        "profile bootstrap input bytes");
    if (packet.value.byte_order != session.header().byte_order)
      throw BootstrapError("profile packet byte order disagrees with stream");
    const Key key{packet.stream_id, packet.instance_id};
    auto found = builds.find(key);
    if (found == builds.end()) {
      if (builds.size() >= options.max_sources)
        throw BootstrapError("profile bootstrap source budget exceeded");
      found = builds.emplace(key, Build{}).first;
      found->second.property = packet.property_name;
    } else if (found->second.property != packet.property_name)
      found->second.ambiguous_property = true;
  }
  std::map<std::uint64_t, const ObjectLifetime *> live;
  for (const auto &entry : builds)
    live.emplace(entry.first.second, nullptr);
  for (const auto &life : session.lifetimes())
    if (life.created_event < prefix &&
        (!life.destroyed_event || *life.destroyed_event >= prefix)) {
      const auto found = live.find(life.id);
      if (found != live.end())
        found->second = &life;
    }
  for (auto &entry : builds)
    entry.second.lifetime = live.at(entry.first.second);
  Index index(session.header(), options.decode_limits);
  index.update(session, prefix);
  if (index.statistics().saturated || index.statistics().suppressed_diagnostics)
    throw BootstrapError("profile index limits prevent trustworthy bootstrap");
  std::map<std::uint64_t, Key> zone_source;
  for (auto &entry : builds) {
    auto &build = entry.second;
    if (!build.lifetime)
      continue;
    if (build.ambiguous_property)
      throw BootstrapError("one live profile source uses multiple properties");
    if (!zone_source.emplace(entry.first.second, entry.first).second)
      throw BootstrapError(
          "one live profile zone uses multiple streams; recursive start order "
          "cannot be preserved in one bootstrap packet per source");
    bool found = false;
    for (const auto &zone : index.zones())
      if (zone.first.id == entry.first.second) {
        build.zone = zone.first;
        found = true;
      }
    if (!found)
      throw BootstrapError("live profile source has no zone metadata");
  }
  for (const auto &diagnostic : index.diagnostics()) {
    const auto source = zone_source.find(diagnostic.source.zone.id);
    if (source == zone_source.end())
      continue;
    const auto &build = builds.at(source->second);
    if (!(diagnostic.source.zone == build.zone))
      continue;
    if (diagnostic.code != DiagnosticCode::legacy_value_truncation &&
        diagnostic.code != DiagnosticCode::value_without_timestamp &&
        diagnostic.code != DiagnosticCode::invalid_clock)
      throw BootstrapError("profile bootstrap refused event " +
                           std::to_string(diagnostic.source.event_index) +
                           " +" +
                           std::to_string(diagnostic.source.payload_offset) +
                           ": " + diagnostic.message);
    result.diagnostics.push_back(diagnostic);
  }
  std::map<Lane, std::vector<const Event *>> open;
  std::size_t counter_count = 0;
  for (const auto &event : index.events()) {
    const auto source = zone_source.find(event.source.zone.id);
    if (source == zone_source.end())
      continue;
    auto &build = builds.at(source->second);
    if (!(event.source.zone == build.zone) ||
        event.source.event_index < build.lifetime->created_event)
      continue;
    const Lane lane{event.source.zone, event.thread_id, event.context_id,
                    event.event_id};
    if (event.kind == Kind::start)
      open[lane].push_back(&event);
    else if (event.kind == Kind::stop) {
      auto found = open.find(lane);
      if (found == open.end() || found->second.empty())
        throw BootstrapError("profile bootstrap has an unmatched stop");
      found->second.pop_back();
      if (found->second.empty())
        open.erase(found);
    } else if (event.kind == Kind::value) {
      if (build.counters.emplace(lane, &event).second)
        add(counter_count, 1, options.max_counters,
            "profile bootstrap counter count");
      else
        build.counters[lane] = &event;
    } else
      throw BootstrapError(
          "GPU profile state cannot be bootstrapped as CPU data");
  }
  std::size_t open_count = 0;
  for (const auto &lane : open)
    for (const auto *event : lane.second) {
      add(open_count, 1, options.max_open_scopes,
          "profile bootstrap open scopes");
      builds.at(zone_source.at(event->source.zone.id)).starts.push_back(event);
    }
  for (auto &entry : builds) {
    auto &build = entry.second;
    if (!build.lifetime)
      continue;
    std::sort(build.starts.begin(), build.starts.end());
    std::size_t size = 0;
    for (std::size_t i = 0; i < build.starts.size(); ++i)
      add(size, 26, options.decode_limits.max_packet_bytes,
          "canonical profile packet");
    for (std::size_t i = 0; i < build.counters.size(); ++i)
      add(size, 24, options.decode_limits.max_packet_bytes,
          "canonical profile packet");
    if (size > std::numeric_limits<std::uint32_t>::max())
      throw BootstrapError("canonical profile packet exceeds uint32 items");
    add(result.total_bytes, size, options.max_total_bytes,
        "profile bootstrap output bytes");
    Writer writer{session.header().byte_order, {}};
    writer.bytes.reserve(size);
    SourceBootstrap output;
    output.stream_id = entry.first.first;
    output.zone = build.zone;
    output.buffer_generation = build.lifetime->generation;
    output.property_name = build.property;
    for (const auto *event : build.starts) {
      output.records.push_back(
          {writer.bytes.size(), Kind::start, event->source});
      writer.start(*event);
    }
    std::vector<const Event *> counters;
    for (const auto &counter : build.counters)
      counters.push_back(counter.second);
    std::sort(counters.begin(), counters.end());
    for (const auto *event : counters) {
      output.records.push_back(
          {writer.bytes.size(), Kind::value, event->source});
      writer.value(*event);
    }
    output.bytes = std::move(writer.bytes);
    output.open_scopes = build.starts.size();
    output.counters = build.counters.size();
    result.sources.push_back(std::move(output));
  }
  return result;
}
} // namespace pvd::profile
