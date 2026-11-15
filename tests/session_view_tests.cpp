#include "pvd/coordinates.hpp"
#include "pvd/session.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#if defined(PVD_CORE_STATIC) && (defined(__clang__) || defined(__GNUC__)) &&   \
    !(defined(_MSC_VER) && defined(_DEBUG))
#define VIEW_FAULT_TESTS 1
namespace fault {
bool active = false;
std::size_t calls = 0, point = 0;
bool count = false;
std::size_t allocations = 0;
} // namespace fault
void *operator new(std::size_t size) {
  if (fault::count)
    ++fault::allocations;
  if (fault::active && ++fault::calls == fault::point)
    throw std::bad_alloc();
  if (auto *p = std::malloc(size ? size : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#else
#define VIEW_FAULT_TESTS 0
#endif
namespace {
using namespace pvd;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void equal(const Snapshot &a, const Snapshot &b) {
  check(a.event_count == b.event_count &&
            a.instances.size() == b.instances.size() &&
            a.cameras.size() == b.cameras.size() &&
            a.origin_shifts.size() == b.origin_shifts.size(),
        "snapshot structure");
  for (const auto &entry : a.instances) {
    const auto *y = b.instance(entry.first);
    const auto &x = entry.second;
    check(y && x.id == y->id && x.generation == y->generation &&
              x.creating_stream == y->creating_stream &&
              x.class_name == y->class_name && x.pickable == y->pickable &&
              x.top_level == y->top_level &&
              x.coordinate_epoch == y->coordinate_epoch &&
              bool(x.color) == bool(y->color) &&
              x.properties.size() == y->properties.size(),
          "instance state");
    if (x.color)
      check(x.color->r == y->color->r && x.color->g == y->color->g &&
                x.color->b == y->color->b && x.color->a == y->color->a,
            "object color");
    for (const auto &field : x.properties) {
      const auto *v = y->property(field.first);
      const auto &u = field.second;
      check(v && u.incoming_type == v->incoming_type &&
                u.item_count == v->item_count &&
                u.byte_order == v->byte_order &&
                u.pointer_width == v->pointer_width && u.opaque == v->opaque &&
                u.text == v->text && u.reference_ids == v->reference_ids &&
                u.coordinate_epoch == v->coordinate_epoch &&
                u.raw_bytes() == v->raw_bytes() &&
                bool(u.native_coordinate_bytes) ==
                    bool(v->native_coordinate_bytes),
            "property state");
      if (u.native_coordinate_bytes)
        check(*u.native_coordinate_bytes == *v->native_coordinate_bytes,
              "native coordinate bytes");
    }
  }
  for (const auto &camera : a.cameras) {
    const auto &v = b.cameras.at(camera.first);
    check(std::memcmp(&camera.second.position, &v.position, sizeof(Vec3)) ==
                  0 &&
              std::memcmp(&camera.second.up, &v.up, sizeof(Vec3)) == 0 &&
              std::memcmp(&camera.second.target, &v.target, sizeof(Vec3)) == 0,
          "camera state");
  }
  for (const auto &origin : a.origin_shifts) {
    const auto &v = b.origin_shifts.at(origin.first);
    check(std::memcmp(&origin.second, &v, sizeof(Vec3)) == 0, "origin state");
  }
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  explicit Fixture(ByteOrder order, unsigned width) {
    header.byte_order = order;
    header.stream_flags = width == 8 ? 1 : 0;
    std::uint32_t handle = 1;
    for (const char *s : {"physx3", "Owner", "Refs", "ObjectRef", "VoidPtr",
                          "test", "Mass", "PvdF32", "Name", "String"})
      emit(StringHandleEvent{s, handle++});
    emit(CreateClass{{6, 2}});
    emit(CreateProperty{{6, 2}, 3, 0, {1, 4}, 2, {}});
    emit(CreateProperty{{6, 2}, 7, 0, {1, 8}, 1, {}});
    emit(CreateProperty{{6, 2}, 9, 0, {1, 10}, 1, {}});
    for (unsigned i = 1; i <= 3; ++i)
      emit(CreateInstance{{6, 2}, i});
  }
  void emit(Payload payload) {
    events.push_back({1, events.size(), 0, std::move(payload)});
  }
  Bytes integer(std::uint64_t value, unsigned width) const {
    Bytes data;
    for (unsigned n = 0; n < width; ++n)
      data.push_back(static_cast<std::uint8_t>(
          value >>
          (8 * (header.byte_order == ByteOrder::little ? n : width - 1 - n))));
    return data;
  }
  void mass(unsigned id, unsigned value) {
    float f = static_cast<float>(value);
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    emit(SetPropertyValue{id, 7, {1, 8}, 1, integer(bits, 4)});
  }
  void name(unsigned id, const char *text) {
    auto bytes = integer(0, header.pointer_width());
    while (*text)
      bytes.push_back(static_cast<std::uint8_t>(*text++));
    bytes.push_back(0);
    emit(SetPropertyValue{id, 9, {1, 10}, 1, std::move(bytes)});
  }
};
void prefixes(ByteOrder order, unsigned width, std::size_t cache) {
  Fixture f(order, width);
  f.emit(StringHandleEvent{"RefMass", 11});
  f.emit(CreatePropertyMessage{{6, 2},
                               {6, 11},
                               {{3, {1, 5}, 0, width}, {7, {1, 8}, width, 4}},
                               width + 4});
  f.name(1, "unchanged");
  f.mass(2, 1);
  f.name(3, "static");
  for (unsigned n = 0; n < 20; ++n) {
    if (n == 5) {
      Bytes refs;
      for (auto id : {2u, 3u, 2u}) {
        auto value = f.integer(id, width);
        refs.insert(refs.end(), value.begin(), value.end());
      }
      f.emit(SetPropertyValue{1, 3, {1, 5}, 3, std::move(refs)});
    }
    if (n == 12)
      f.emit(SetPropertyValue{1, 3, {1, 5}, 0, {}});
    if (n == 15) {
      auto values = f.integer(3, width);
      float mass = 15;
      std::uint32_t bits;
      std::memcpy(&bits, &mass, 4);
      auto value = f.integer(bits, 4);
      values.insert(values.end(), value.begin(), value.end());
      f.emit(SetPropertyMessage{1, {6, 11}, std::move(values)});
    }
    f.mass(2, n);
    f.emit(PushBackObjectRef{1, 3, n % 3});
    if (n % 3 == 0)
      f.emit(RemoveObjectRef{1, 3, n % 3});
    if (n == 7) {
      f.emit(DestroyInstance{2});
      f.emit(CreateInstance{{6, 2}, 2});
    }
    f.emit(SetPickable{2, n % 2 == 0});
    f.emit(SetColor{2, {static_cast<std::uint8_t>(n), 2, 3, 255}});
  }
  f.emit(SetCamera{"camera", {1, 2, 3}, {0, 1, 0}, {0, 0, 0}});
  SessionOptions options;
  options.checkpoint_interval = 7;
  options.max_checkpoints = 3;
  options.max_view_cache_bytes = cache;
  Session session(f.header, f.events, options);
  std::vector<SnapshotView> retained;
  for (std::size_t count = 0; count <= f.events.size(); ++count) {
    auto view = session.snapshot_view_at_event(count);
    equal(session.snapshot_at_event(count), view.materialize());
    check(view.instances.size() == view.materialize().instances.size(),
          "full object view");
    auto selected = session.snapshot_selected_at_event(count, {1, 2, 2, 999});
    auto wanted = view.select({1, 2, 2, 999});
    equal(wanted, selected);
    for (auto id : {1ull, 2ull, 999ull}) {
      auto object = session.instance_at_event(count, id);
      check(bool(object) == bool(view.instance(id)),
            "instance projection existence");
      if (object)
        check(object->generation == view.instance(id)->generation &&
                  object->properties.size() ==
                      view.instance(id)->properties.size(),
              "instance projection generation");
    }
    retained.push_back(view);
    check(session.estimated_view_cache_bytes() <= cache,
          "instance/ref shared cache budget");
  }
  for (std::size_t count = f.events.size() + 1; count-- > 0;) {
    auto view = session.snapshot_view_at_event(count);
    equal(session.snapshot_at_event(count), view.materialize());
    equal(retained[count].materialize(), view.materialize());
  }
  for (auto count :
       {f.events.size() / 2, std::size_t(0), f.events.size(), std::size_t(20)})
    equal(session.snapshot_at_event(count),
          session.snapshot_view_at_event(count).materialize());
  auto last = session.snapshot_view_at_event(session.event_count());
  auto copy = last;
  SnapshotInstances moved = std::move(copy.instances);
  check(copy.instances.size() == moved.size(),
        "moved-from facade remains valid");
  Snapshot full = last;
  SnapshotView converted(full);
  full.instances.at(1).properties["Name"].text = "changed";
  check(converted.instance(1)->property("Name")->text ==
            std::optional<std::string>("unchanged"),
        "Snapshot conversion leaked mutable alias");
}
void live_and_weak_reuse() {
  Fixture f(ByteOrder::little, 8);
  for (unsigned id = 4; id < 504; ++id) {
    f.emit(CreateInstance{{6, 2}, id});
    f.mass(id, id);
  }
  SessionOptions options;
  options.max_checkpoints = 0;
  options.max_view_cache_bytes = 0;
  Session session(f.header, options);
  session.append(f.events);
  auto before = session.snapshot_view_at_event(session.event_count());
  f.events.clear();
  f.mass(2, 99);
  session.append(f.events);
  auto after = session.snapshot_view_at_event(session.event_count());
  check(before.instance(2) != after.instance(2),
        "changed instance version reused");
  for (unsigned id = 4; id < 504; ++id)
    check(before.instance(id) == after.instance(id),
          "unchanged live instances not shared");
  session.snapshot_view_at_event(0);
  auto restored = session.snapshot_view_at_event(session.event_count());
  for (unsigned id = 4; id < 504; ++id)
    check(before.instance(id) == restored.instance(id),
          "zero-strong-budget view-held weak reuse failed");
  const auto prefix = session.event_count();
  bool rejected = false;
  try {
    session.append({Event{1, 0, 0, PushBackObjectRef{1, 3, 1}},
                    Event{1, 0, 0, RemoveObjectRef{1, 3, 999}}});
  } catch (const SessionError &) {
    rejected = true;
  }
  check(rejected && session.state_available() &&
            session.event_count() == prefix + 1,
        "failed append prefix for view");
  equal(session.latest(),
        session.snapshot_view_at_event(session.event_count()).materialize());
  check(before.instance(1)->property("Refs") == nullptr,
        "failed append mutated historical view");
}
void lifetime() {
  SnapshotView retained;
  {
    Fixture f(ByteOrder::big, 4);
    f.mass(1, 42);
    Session session(f.header, f.events);
    retained = session.snapshot_view_at_event(session.event_count());
  }
  check(retained.instance(1)->property("Mass")->as_float() ==
            std::optional<float>(42.f),
        "view did not outlive Session");
}
void native_fallback(ByteOrder order, unsigned width) {
  Fixture f(order, width);
  std::uint32_t handle = 11;
  for (const char *s :
       {"PxScene", "PxRigidStatic", "Scene", "GlobalPose", "PxTransform"})
    f.emit(StringHandleEvent{s, handle++});
  f.emit(CreateClass{{1, 11}});
  f.emit(CreateClass{{1, 12}});
  f.emit(CreateProperty{{1, 12}, 13, 0, {1, 4}, 1, {}});
  f.emit(CreateProperty{{1, 12}, 14, 0, {1, 15}, 1, {}});
  f.emit(CreateInstance{{1, 11}, 100});
  f.emit(CreateInstance{{1, 12}, 101});
  f.emit(SetPropertyValue{101, 13, {1, 5}, 1, f.integer(100, width)});
  Bytes pose;
  for (float value : {0.f, 0.f, 0.f, 1.f, 100.f, 2.f, 3.f}) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    auto field = f.integer(bits, 4);
    pose.insert(pose.end(), field.begin(), field.end());
  }
  f.emit(SetPropertyValue{101, 14, {1, 15}, 1, pose});
  const auto before = f.events.size();
  f.emit(OriginShift{100, {10, 0, 0}});
  const auto after = f.events.size();
  f.emit(OriginShift{100, {.125f, 0, 0}});
  Session session(f.header, f.events);
  auto old = session.snapshot_view_at_event(before);
  for (auto prefix : {after, f.events.size(), before, after}) {
    auto view = session.snapshot_view_at_event(prefix);
    equal(session.snapshot_at_event(prefix), view.materialize());
    auto selected = view.select_view({101, 101, 999});
    check(selected.instances.size() == 1 &&
              selected.instance(101) == view.instance(101),
          "select_view must share immutable objects without a transitive "
          "closure");
    equal(view.select({101}), selected.materialize());
    equal(session.snapshot_selected_at_event(prefix, {101}),
          selected.materialize());
  }
  check(coordinates::native_transform(
            *session.snapshot_view_at_event(after).instance(101)->property(
                "GlobalPose"))
                ->position.x == 90,
        "view origin native shadow");
  check(
      coordinates::native_transform(*old.instance(101)->property("GlobalPose"))
              ->position.x == 100,
      "published pre-shift view changed");
}
void query_allocation_failures() {
#if VIEW_FAULT_TESTS
  std::size_t rejected_queries = 0;
  for (bool warm : {false, true}) {
    bool success = false;
    for (std::size_t point = 1; point < 300; ++point) {
      Fixture f(ByteOrder::little, 8);
      f.mass(2, 1);
      f.name(1, "steady");
      f.emit(PushBackObjectRef{1, 3, 2});
      SessionOptions options;
      options.max_view_cache_bytes = 700;
      Session session(f.header, options);
      session.append(f.events);
      SnapshotView retained;
      if (warm) {
        retained = session.snapshot_view_at_event(session.event_count());
        f.events.clear();
        f.mass(2, 99);
        f.emit(PushBackObjectRef{1, 3, 3});
        session.append(f.events);
      }
      const auto reference = session.latest();
      fault::calls = 0;
      fault::point = point;
      fault::active = true;
      bool failed = false;
      try {
        session.snapshot_view_at_event(session.event_count());
      } catch (const std::bad_alloc &) {
        failed = true;
      } catch (...) {
        fault::active = false;
        throw;
      }
      fault::active = false;
      check(!session.failed() && session.state_available(),
            "failed view allocation changed Session status");
      equal(
          reference,
          session.snapshot_view_at_event(session.event_count()).materialize());
      if (warm)
        check(retained.instance(2)->property("Mass")->as_float() ==
                  std::optional<float>(1.f),
              "failed view query mutated retained instance");
      if (failed)
        ++rejected_queries;
      else {
        success = true;
        break;
      }
    }
    check(success, "view query allocation fault range incomplete");
  }
  check(rejected_queries > 20,
        "view cache/index allocation failures not exercised");
  std::cout << "view query allocation failure points=" << rejected_queries
            << '\n';
#endif
}
void concurrent_queries() {
  Fixture fixture(ByteOrder::little, 8);
  for (unsigned n = 0; n < 40; ++n) {
    fixture.mass(2, n);
    fixture.emit(PushBackObjectRef{1, 3, n % 3});
  }
  SessionOptions options;
  options.max_view_cache_bytes = 700;
  Session session(fixture.header, fixture.events, options);
  std::vector<Snapshot> reference;
  for (std::size_t i = 0; i <= session.event_count(); ++i)
    reference.push_back(session.snapshot_at_event(i));
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> readers;
  for (unsigned reader = 0; reader < 4; ++reader)
    readers.emplace_back([&, reader] {
      try {
        for (unsigned i = 0; i < 80; ++i) {
          const auto count = (i * 37 + reader * 11) % reference.size();
          auto view = session.snapshot_view_at_event(count);
          equal(reference[count], view.materialize());
          auto selected = session.snapshot_selected_at_event(count, {1, 2});
          equal(view.select({1, 2}), selected);
        }
      } catch (...) {
        ++failures;
      }
    });
  for (auto &reader : readers)
    reader.join();
  check(!failures.load(),
        "concurrent const snapshot/view/selected queries diverged");
}
void long_class_budget() {
  StreamHeader header;
  const std::string space(4096, 'n'), name(4096, 'c');
  std::vector<Event> events{{1, 0, 0, StringHandleEvent{space, 1}},
                            {1, 0, 0, StringHandleEvent{name, 2}},
                            {1, 0, 0, CreateClass{{1, 2}}},
                            {1, 0, 0, CreateInstance{{1, 2}, 99}}};
  SessionOptions options;
  options.max_checkpoints = 0;
  options.max_view_cache_bytes = 1024;
  Session session(header, events, options);
  const auto view = session.snapshot_view_at_event(session.event_count());
  check(view.instance(99) &&
            view.instance(99)->class_name == QualifiedName{space, name},
        "long class namespace/name lost");
  check(session.estimated_view_cache_bytes() == 0,
        "tiny strong cache retained an oversized class namespace/name");
}
void flat_index_allocation_guard() {
#if VIEW_FAULT_TESTS
  Fixture fixture(ByteOrder::little, 8);
  for (unsigned id = 4; id < 4100; ++id)
    fixture.emit(CreateInstance{{6, 2}, id});
  SessionOptions options;
  options.max_checkpoints = 0;
  Session session(fixture.header, options);
  session.append(fixture.events);
  const auto before = session.snapshot_view_at_event(session.event_count());
  fixture.events.clear();
  fixture.mass(2, 17);
  session.append(fixture.events);
  fault::allocations = 0;
  fault::count = true;
  SnapshotView after;
  try {
    after = session.snapshot_view_at_event(session.event_count());
  } catch (...) {
    fault::count = false;
    throw;
  }
  fault::count = false;
  check(fault::allocations < 128,
        "continuous view allocated one tree node per retained object");
  check(before.instance(4) == after.instance(4) &&
            before.instance(2) != after.instance(2),
        "flat index sharing");
  auto selected = after.select_view({500, 4, 2, 4});
  check(selected.instances.size() == 3 &&
            selected.instances.begin()->first == 2 &&
            selected.instances.lower_bound(3)->first == 4 &&
            selected.instances.find(999) == selected.instances.end(),
        "flat index sorted/unique/find/lower_bound");
  bool rejected = false;
  try {
    selected.instances.at(999);
  } catch (const std::out_of_range &) {
    rejected = true;
  }
  check(rejected, "flat index at missing ID");
  std::cout << "continuous view index allocations=" << fault::allocations
            << " objects=" << after.instances.size() << '\n';
#endif
}
static_assert(
    !std::is_constructible_v<SnapshotInstances,
                             std::shared_ptr<SnapshotInstances::Storage>>,
    "mutable storage alias constructor must stay private");
static_assert(std::is_const_v<std::remove_reference_t<
                  decltype(std::declval<SnapshotInstances>().at(0))>>,
              "view at must be immutable");
} // namespace
int main() {
  try {
    for (auto order : {ByteOrder::little, ByteOrder::big})
      for (unsigned width : {4u, 8u}) {
        for (std::size_t budget : {std::size_t(0), std::size_t(1024),
                                   std::size_t(128u * 1024u * 1024u)})
          prefixes(order, width, budget);
        native_fallback(order, width);
      }
    live_and_weak_reuse();
    lifetime();
    query_allocation_failures();
    concurrent_queries();
    long_class_budget();
    flat_index_allocation_guard();
    std::cout << "snapshot views: full/projection prefix equality, ABI, "
                 "reverse/live, immutable versions, weak reuse and Session "
                 "lifetime passed\n";
    return 0;
  } catch (const std::exception &e) {
#if VIEW_FAULT_TESTS
    fault::active = false;
#endif
    std::cerr << e.what() << '\n';
    return 1;
  }
}
