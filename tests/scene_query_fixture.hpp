#pragma once
// Test-only wire fixture derived from the verified PhysX 3.4 native structs.
// Shared by core and actual-ImGui tests; it does not call the implementation.
#include "pvd/scene_queries.hpp"
#include <cstring>
#include <map>
namespace scene_query_test {
using namespace pvd;
struct BytesWriter {
  ByteOrder order;
  Bytes bytes;
  explicit BytesWriter(ByteOrder o) : order(o) {}
  void u(std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          value >> (8 * (order == ByteOrder::little ? i : width - i - 1))));
  }
  void at(std::size_t offset, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.at(offset + i) = static_cast<std::uint8_t>(
          value >> (8 * (order == ByteOrder::little ? i : width - i - 1)));
  }
  void f(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    u(bits, 4);
  }
  void v(Vec3 value) {
    f(value.x);
    f(value.y);
    f(value.z);
  }
  void pose(Vec3 position = {2, 3, 4}) {
    f(0);
    f(0);
    f(0);
    f(1);
    v(position);
  }
  void pad(std::size_t end) { bytes.resize(end, 0xa5); }
  void text(const std::string &value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
    u(0, 1);
  }
  void ref(std::size_t width, std::uint32_t base, std::uint32_t count) {
    u(width == 8 ? 0xfedcba9876543210ull : 0xdeadbeefu, width);
    u(base, 4);
    u(count, 4);
  }
};
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::uint64_t, std::map<std::string, std::uint32_t>> handles;
  std::uint64_t stream = 17, scene = 101, other_scene = 102, actor = 201,
                shape = 301, box = 401;
  bool geometry_present[2]{};
  explicit Fixture(ByteOrder order = ByteOrder::little,
                   std::uint8_t pointers = 8) {
    header.byte_order = order;
    header.stream_flags = pointers == 8 ? 1 : 0;
    if (pointers == 8) {
      const auto high = 0xfedcba9800000000ull;
      scene |= high;
      other_scene |= high;
      actor |= high;
      shape |= high;
      box |= high;
    }
    for (const char *name :
         {"ObjectRef", "VoidPtr", "PvdF32", "PxVec3", "PxTransform",
          "U32Array4", "PvdRaycast", "PvdSweep", "PvdOverlap", "PvdSqHit",
          "PxScene", "PxActor", "PxShape", "PxBoxGeometry"})
      emit(CreateClass{type(name)});
    property("PxActor", "Scene", "ObjectRef");
    property("PxBoxGeometry", "HalfExtents", "PxVec3");
    for (bool batch : {false, true}) {
      const std::string prefix = batch ? "BatchedQueries." : "SceneQueries.";
      property("PxScene", prefix + "Raycasts", "PvdRaycast", 2);
      property("PxScene", prefix + "Sweeps", "PvdSweep", 2);
      property("PxScene", prefix + "Overlaps", "PvdOverlap", 2);
      property("PxScene", prefix + "Hits", "PvdSqHit", 2);
      property("PxScene", prefix + "PoseList", "PxTransform", 2);
      property("PxScene", prefix + "FilterDataList", "U32Array4", 2);
      property("PxScene", prefix + "GeometryList", "ObjectRef", 2);
    }
    emit(CreateInstance{type("PxScene"), scene});
    emit(CreateInstance{type("PxScene"), other_scene});
    emit(CreateInstance{type("PxActor"), actor});
    emit(CreateInstance{type("PxShape"), shape});
    emit(CreateInstance{type("PxBoxGeometry"), box});
    actor_scene(scene);
    BytesWriter ext(order);
    ext.v({1, 2, 3});
    set(box, "HalfExtents", "PxVec3", ext.bytes, 1);
  }
  template <class T> void emit(T value) {
    events.push_back({stream, events.size() * 17, events.size() * 23,
                      Payload{std::move(value)}});
  }
  std::uint32_t str(const std::string &value) {
    if (value.empty())
      return 0;
    auto &map = handles[stream];
    const auto found = map.find(value);
    if (found != map.end())
      return found->second;
    const auto h = static_cast<std::uint32_t>(map.size() + 1);
    map.emplace(value, h);
    emit(StringHandleEvent{value, h});
    return h;
  }
  WireName type(const std::string &name) { return {str("physx3"), str(name)}; }
  void property(const std::string &cls, const std::string &name,
                const std::string &datatype, std::uint8_t kind = 1) {
    const auto c = type(cls), d = type(datatype);
    const auto h = str(name);
    emit(CreateProperty{c, h, 0, d, kind, {}});
  }
  void set(std::uint64_t id, const std::string &name,
           const std::string &datatype, Bytes bytes, std::uint32_t count) {
    const auto h = str(name);
    const auto t = type(datatype);
    emit(SetPropertyValue{id, h, t, count, std::move(bytes)});
  }
  void chunk(std::uint64_t id, const std::string &name,
             const std::string &datatype, Bytes bytes, std::uint32_t count) {
    const auto h = str(name);
    const auto t = type(datatype);
    emit(BeginSetPropertyValue{id, h, t});
    emit(AppendPropertyValueData{std::move(bytes), count});
    emit(EndSetPropertyValue{});
  }
  void actor_scene(std::uint64_t id) {
    BytesWriter w(header.byte_order);
    w.u(id, header.pointer_width());
    set(actor, "Scene", "VoidPtr", w.bytes, 1);
  }
  std::string prefix(bool batch) const {
    return batch ? "BatchedQueries." : "SceneQueries.";
  }
  Bytes hit(std::uint32_t flags = 1039, std::uint64_t actor_id = 0,
            std::uint64_t shape_id = 0, float distance = 5,
            Vec3 position = {1, 2, 3}, Vec3 normal = {0, 1, 0},
            float uv = .25f) const {
    BytesWriter w(header.byte_order);
    w.u(shape_id ? shape_id : shape, header.pointer_width());
    w.u(actor_id ? actor_id : actor, header.pointer_width());
    w.u(0xf1234567u, 4);
    w.u(flags, 4);
    w.v(position);
    w.v(normal);
    w.f(distance);
    w.f(uv);
    w.f(.75f);
    w.pad(header.pointer_width() == 8 ? 64 : 52);
    return w.bytes;
  }
  Bytes ray(bool batch = false, std::uint32_t type_id = 1,
            std::uint32_t base = 0, std::uint32_t count = 1,
            std::uint32_t flags = 3, Vec3 origin = {-3, 2, 3},
            Vec3 direction = {1, 0, 0}, float distance = 9,
            const std::string &array_name = {}) const {
    BytesWriter w(header.byte_order);
    w.u(type_id, 4);
    for (unsigned i = 1; i <= 4; ++i)
      w.u(0xf1234500u + i, 4);
    w.u(flags, 4);
    w.v(origin);
    w.v(direction);
    w.f(distance);
    w.pad(header.pointer_width() == 8 ? 56 : 52);
    w.ref(header.pointer_width(), base, count);
    w.pad(header.pointer_width() == 8 ? 72 : 64);
    w.text(array_name.empty() ? prefix(batch) + "Hits" : array_name);
    return w.bytes;
  }
  Bytes sweep(bool batch = false) const {
    BytesWriter w(header.byte_order);
    w.u(8, 4);
    w.u(3, 4);
    w.v({0, 0, 1});
    w.f(4);
    for (int i = 0; i < 4; ++i)
      w.ref(header.pointer_width(), 0, 1);
    w.pad(header.pointer_width() == 8 ? 88 : 72);
    for (const char *name :
         {"GeometryList", "PoseList", "FilterDataList", "Hits"})
      w.text(prefix(batch) + name);
    return w.bytes;
  }
  Bytes overlap(bool batch = false) const {
    BytesWriter w(header.byte_order);
    w.u(4, 4);
    for (unsigned i = 1; i <= 4; ++i)
      w.u(0xf1234500u + i, 4);
    w.u(35, 4);
    w.pose({10, 20, 30});
    w.pad(header.pointer_width() == 8 ? 56 : 52);
    w.ref(header.pointer_width(), 0, 1);
    w.ref(header.pointer_width(), 2, 1);
    w.pad(header.pointer_width() == 8 ? 88 : 76);
    w.text(prefix(batch) + "GeometryList");
    w.text(prefix(batch) + "Hits");
    return w.bytes;
  }
  void auxiliaries(bool batch = false, Bytes hits = {},
                   std::uint32_t count = 1) {
    const auto p = prefix(batch);
    if (hits.empty())
      hits = hit();
    chunk(scene, p + "Hits", "PvdSqHit", std::move(hits), count);
    BytesWriter pose(header.byte_order), filter(header.byte_order);
    pose.pose();
    for (unsigned i = 1; i <= 4; ++i)
      filter.u(0xf1234500u + i, 4);
    set(scene, p + "PoseList", "PxTransform", pose.bytes, 1);
    set(scene, p + "FilterDataList", "U32Array4", filter.bytes, 1);
    const auto property = str(p + "GeometryList");
    if (geometry_present[batch ? 1 : 0])
      emit(RemoveObjectRef{scene, property, box});
    emit(PushBackObjectRef{scene, property, box});
    geometry_present[batch ? 1 : 0] = true;
  }
  void publish(bool batch = false) {
    auto hits = hit();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    auto second = hit(0, 0, 0, 7, {nan, nan, nan}, {nan, nan, nan}, nan);
    auto overlap_hit = hit(0, 0, 0, 0, {}, {}, 0);
    hits.insert(hits.end(), second.begin(), second.end());
    hits.insert(hits.end(), overlap_hit.begin(), overlap_hit.end());
    auxiliaries(batch, hits, 3);
    auto rays = ray(batch), second_ray = ray(batch, 2, 1, 1, 35);
    rays.insert(rays.end(), second_ray.begin(), second_ray.end());
    chunk(scene, prefix(batch) + "Raycasts", "PvdRaycast", rays, 2);
    set(scene, prefix(batch) + "Sweeps", "PvdSweep", sweep(batch), 1);
    chunk(scene, prefix(batch) + "Overlaps", "PvdOverlap", overlap(batch), 1);
  }
  Session session() const { return Session(header, events); }
};
} // namespace scene_query_test
