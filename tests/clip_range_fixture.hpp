#pragma once
#include "pvd/session.hpp"
#include <map>

namespace clip_test {
using namespace pvd;
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  std::size_t live_seed = 0, open_frame = 0, first_value = 0;
  void emit(Payload payload) {
    events.push_back(
        {17, events.size() * 11, events.size() * 23, std::move(payload)});
  }
  std::uint32_t str(const std::string &value) {
    if (value.empty())
      return 0;
    const auto found = strings.find(value);
    if (found != strings.end())
      return found->second;
    const auto handle = static_cast<std::uint32_t>(strings.size() + 1);
    strings.emplace(value, handle);
    emit(StringHandleEvent{value, handle});
    return handle;
  }
  WireName type(const char *value) {
    const auto ns = str("physx3"), name = str(value);
    return {ns, name};
  }
  Bytes integer(std::uint64_t value) const {
    Bytes bytes;
    for (unsigned i = 0; i < 8; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          value >> (8 * (header.byte_order == ByteOrder::little ? i : 7 - i))));
    return bytes;
  }
  void frame(std::uint64_t scene, unsigned index) {
    emit(BeginSection{scene, str("frame"), index * 100});
    if (scene == 1 && index == 3)
      open_frame = events.size();
    const auto property = str("Counter");
    const auto datatype = type("PvdU64");
    if (!first_value)
      first_value = events.size();
    emit(SetPropertyValue{20, property, datatype, 1,
                          integer(9007199254740993ull + index)});
    emit(EndSection{scene, str("frame"), index * 100 + 90});
    emit(BeginSection{scene, str("validation_after_fetch"), index * 100 + 91});
    emit(EndSection{scene, str("validation_after_fetch"), index * 100 + 92});
  }
  explicit Fixture(ByteOrder order = ByteOrder::little) {
    header.byte_order = order;
    header.stream_flags = 1;
    emit(CreateClass{type("PxScene")});
    emit(CreateClass{type("ClipNode")});
    const auto cls = type("ClipNode"), datatype = type("PvdU64");
    emit(CreateProperty{cls, str("Counter"), 0, datatype, 1, {}});
    emit(CreateInstance{type("PxScene"), 1});
    emit(CreateInstance{type("PxScene"), 2});
    emit(CreateInstance{cls, 20});
    // Resolve strings before recording the seed boundary.
    str("frame");
    str("validation_after_fetch");
    for (unsigned i = 0; i < 6; ++i) {
      frame(1, i);
      if (i % 2)
        frame(2, i / 2);
      if (i == 2)
        live_seed = events.size();
    }
    emit(DestroyInstance{1});
    emit(CreateInstance{type("PxScene"), 1});
    frame(1, 6);
    frame(1, 7);
  }
  Session session() const { return Session(header, events); }
};
} // namespace clip_test
