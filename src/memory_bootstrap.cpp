#include "pvd/memory_bootstrap.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <sstream>

namespace pvd::memory {
namespace {
void add(std::size_t &n, std::size_t amount, std::size_t limit,
         const char *name) {
  if (n > limit || amount > limit - n)
    throw BootstrapError(std::string(name) + " budget exceeded");
  n += amount;
}
std::string location(const Diagnostic &d) {
  return "memory bootstrap refused event " + std::to_string(d.event_index) +
         " source " + std::to_string(d.source.stream_id) + "/" +
         std::to_string(d.source.buffer_id) + " +" +
         std::to_string(d.payload_offset) + ": " + d.message;
}
struct Build {
  std::string property;
  const ObjectLifetime *lifetime = nullptr;
  bool ambiguous_property = false;
  std::size_t encoded_size = 0;
  std::map<std::uint32_t, const std::string *> strings;
  std::vector<const Allocation *> active;
};
struct Writer {
  ByteOrder order;
  Bytes bytes;
  void u(std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          value >> ((order == ByteOrder::little ? i : width - i - 1) * 8)));
  }
  void string(std::uint32_t handle, const std::string &value) {
    u(0x3ff1, 2);
    u(value.size() + 1, 4);
    bytes.insert(bytes.end(), value.begin(), value.end());
    u(0, 1);
    u(handle, 4);
  }
  void allocation(const Allocation &a) {
    // Header compression U64 for every field; SDK u32 fields remain four bytes.
    u(0x3ff2, 2);
    u(a.size, 4);
    u(a.type_handle, 4);
    u(a.file_handle, 4);
    u(a.line, 4);
    u(a.address, 8);
  }
};
} // namespace
Bootstrap make_bootstrap(const Session &session, std::size_t prefix,
                         const BootstrapOptions &options) {
  if (prefix > session.event_count())
    throw BootstrapError("memory bootstrap prefix exceeds Session event count");
  Bootstrap result;
  result.event_count = prefix;
  std::map<Source, Build> builds;
  std::size_t input_bytes = 0;
  for (const auto &packet : session.channels()) {
    if (packet.event_index >= prefix)
      break;
    if (packet.semantic != "memory event stream")
      continue;
    add(input_bytes, packet.value.raw_bytes().size(), options.max_input_bytes,
        "memory bootstrap input bytes");
    const Source source{packet.stream_id, packet.instance_id};
    auto found = builds.find(source);
    if (found == builds.end()) {
      if (builds.size() >= options.max_sources)
        throw BootstrapError("memory bootstrap source budget exceeded");
      found = builds.emplace(source, Build{}).first;
      found->second.property = packet.property_name;
    } else if (found->second.property != packet.property_name)
      found->second.ambiguous_property = true;
    if (packet.value.byte_order != session.header().byte_order)
      throw BootstrapError(
          "memory packet byte order disagrees with source stream");
  }
  // Read lifecycle metadata without copying the complete object Snapshot.
  std::map<std::uint64_t, const ObjectLifetime *> live;
  for (const auto &entry : builds)
    live.emplace(entry.first.buffer_id, nullptr);
  for (const auto &lifetime : session.lifetimes()) {
    const auto found = live.find(lifetime.id);
    if (found != live.end() && lifetime.created_event < prefix &&
        (!lifetime.destroyed_event || *lifetime.destroyed_event >= prefix))
      found->second = &lifetime;
  }
  for (auto &entry : builds) {
    entry.second.lifetime = live.at(entry.first.buffer_id);
    if (entry.second.lifetime && entry.second.ambiguous_property)
      throw BootstrapError("one live memory source uses multiple properties; "
                           "dictionary identity is ambiguous");
    if (entry.second.lifetime && entry.second.lifetime->generation)
      throw BootstrapError("memory buffer instance ID was reused; source "
                           "dictionary generation is ambiguous");
  }
  Index index(options.decode_limits);
  index.update(session, prefix);
  for (const auto &d : index.diagnostics()) {
    if (d.event_index >= prefix)
      continue;
    const auto source = builds.find(d.source);
    if (source == builds.end() || !source->second.lifetime)
      continue;
    if (d.code != DiagnosticCode::unknown_free ||
        options.unknown_frees != UnknownFreePolicy::allow_partial)
      throw BootstrapError(location(d));
    result.partial = true;
    result.diagnostics.push_back(d);
  }
  std::size_t dictionary_entries = 0, active_allocations = 0;
  for (const auto &event : index.events()) {
    if (event.event_index >= prefix)
      break;
    if (event.kind != EventKind::string_table)
      continue;
    auto &build = builds.at(event.source);
    if (!build.lifetime)
      continue;
    if (build.strings.emplace(event.string_handle, &event.string_value).second)
      add(dictionary_entries, 1, options.max_dictionary_entries,
          "memory bootstrap dictionary entries");
  }
  for (const auto &allocation : index.allocations()) {
    if (allocation.allocated_event_count > prefix ||
        (allocation.ended_event_count &&
         *allocation.ended_event_count <= prefix))
      continue;
    auto &build = builds.at(allocation.source);
    if (!build.lifetime)
      continue;
    add(active_allocations, 1, options.max_active_allocations,
        "memory bootstrap active allocations");
    // Earlier undefined strings have already failed above, but validate the
    // retained state independently before producing any synthetic bytes.
    if (!allocation.type_name || !allocation.file ||
        !build.strings.count(allocation.type_handle) ||
        !build.strings.count(allocation.file_handle) ||
        allocation.size > std::numeric_limits<std::uint32_t>::max())
      throw BootstrapError(
          "memory bootstrap allocation is missing a validated wire field");
    build.active.push_back(&allocation);
  }
  // Preflight the complete canonical output before allocating output payloads.
  for (auto &entry : builds) {
    auto &build = entry.second;
    if (!build.lifetime)
      continue;
    const auto packet_limit =
        std::min<std::size_t>(options.decode_limits.max_packet_bytes,
                              std::numeric_limits<std::uint32_t>::max());
    for (const auto &string : build.strings) {
      const auto size = string.second->size();
      if (size >= std::numeric_limits<std::uint32_t>::max())
        throw BootstrapError("memory bootstrap string exceeds u32 length");
      add(build.encoded_size, size, packet_limit,
          "canonical memory packet bytes");
      add(build.encoded_size, 11, packet_limit,
          "canonical memory packet bytes");
      add(result.total_bytes, size, options.max_total_bytes,
          "memory bootstrap output bytes");
      add(result.total_bytes, 11, options.max_total_bytes,
          "memory bootstrap output bytes");
    }
    for (const auto *allocation : build.active) {
      (void)allocation;
      add(build.encoded_size, 26, packet_limit,
          "canonical memory packet bytes");
      add(result.total_bytes, 26, options.max_total_bytes,
          "memory bootstrap output bytes");
    }
  }
  for (auto &entry : builds) {
    auto &build = entry.second;
    if (!build.lifetime)
      continue;
    SourceBootstrap source;
    source.source = entry.first;
    source.buffer_generation = build.lifetime->generation;
    source.property_name = build.property;
    source.dictionary_entries = build.strings.size();
    source.active_allocations = build.active.size();
    Writer writer{session.header().byte_order, {}};
    writer.bytes.reserve(build.encoded_size);
    for (const auto &string : build.strings)
      writer.string(string.first, *string.second);
    std::sort(
        build.active.begin(), build.active.end(),
        [](const auto *a, const auto *b) { return a->address < b->address; });
    for (const auto *allocation : build.active)
      writer.allocation(*allocation);
    source.bytes = std::move(writer.bytes);
    result.sources.push_back(std::move(source));
  }
  return result;
}
} // namespace pvd::memory
