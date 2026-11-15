#include "pvd/session.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

// Allocation volume, not RSS or timing: small bounded workloads detect the
// quadratic regression before running the larger real-scene-sized workload.
namespace allocations {
bool active = false;
std::uint64_t bytes = 0;
bool fault = false;
std::size_t calls = 0, fail_at = 0;
} // namespace allocations
#ifdef PVD_CORE_STATIC
void *operator new(std::size_t size) {
#if (defined(__clang__) || defined(__GNUC__)) &&                               \
    !(defined(_MSC_VER) && defined(_DEBUG))
  if (allocations::fault && ++allocations::calls == allocations::fail_at)
    throw std::bad_alloc();
#endif
  if (allocations::active)
    allocations::bytes += size;
  if (auto *p = std::malloc(size ? size : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#endif
namespace {
using namespace pvd;
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  explicit Fixture(ByteOrder order = ByteOrder::little, unsigned width = 8) {
    header.byte_order = order;
    header.stream_flags = width == 8 ? 1 : 0;
    dictionary(1);
    emit(1, CreateClass{{6, 2}});
    emit(1, CreateProperty{{6, 2}, 3, 0, {1, 4}, 2, {}});
    emit(1, CreateInstance{{6, 2}, 7});
  }
  void emit(std::uint64_t stream, Payload payload) {
    events.push_back({stream, events.size(), 0, std::move(payload)});
  }
  void dictionary(std::uint64_t stream) {
    std::uint32_t handle = 1;
    for (const char *s : {"physx3", "Owner", "Refs", "ObjectRef", "VoidPtr",
                          "test", "RefMessage"})
      emit(stream, StringHandleEvent{s, handle++});
  }
  Bytes bytes(const std::vector<std::uint64_t> &ids) const {
    Bytes result;
    for (auto id : ids)
      for (unsigned i = 0; i < header.pointer_width(); ++i)
        result.push_back(static_cast<std::uint8_t>(
            id >> (8 * (header.byte_order == ByteOrder::little
                            ? i
                            : header.pointer_width() - i - 1))));
    return result;
  }
  void assign(std::vector<std::uint64_t> ids) {
    auto count = static_cast<std::uint32_t>(ids.size());
    emit(1, SetPropertyValue{7, 3, {1, 5}, count, bytes(ids)});
  }
};
void expected(const Snapshot &snapshot, const Fixture &f,
              const std::vector<std::uint64_t> &ids) {
  const auto *object = snapshot.instance(7);
  check(object, "live owner missing");
  const auto *p = object->property("Refs");
  check(p && p->reference_ids && *p->reference_ids == ids &&
            p->item_count == ids.size(),
        "reference order/count/duplicates changed");
  check(p->raw_bytes() == f.bytes(ids),
        "reference wire bytes are not materialized with source ABI");
  check(p->incoming_type == QualifiedName{"physx3", "VoidPtr"} &&
            p->pointer_width == f.header.pointer_width() &&
            p->byte_order == f.header.byte_order && !p->opaque,
        "canonical reference value type/ABI changed");
}
void semantics(ByteOrder order, unsigned width, bool checkpoints) {
  Fixture f(order, width);
  f.assign({11, 22, 22});
  SessionOptions options;
  options.checkpoint_interval = checkpoints ? 3 : 0;
  options.max_checkpoints = 4;
  Session live(f.header, options);
  live.append(f.events);
  const auto before = live.latest();
  const auto before_count = live.event_count();
  f.events.clear();
  std::vector<std::pair<std::size_t, std::vector<std::uint64_t>>> states;
  const auto state = [&](std::vector<std::uint64_t> refs) {
    states.push_back({before_count + f.events.size(), std::move(refs)});
  };
  f.emit(1, PushBackObjectRef{7, 3, 33});
  state({11, 22, 22, 33});
  f.emit(1, RemoveObjectRef{7, 3, 22});
  state({11, 22, 33});
  f.emit(1, PushBackObjectRef{7, 3, 11});
  state({11, 22, 33, 11});
  f.assign({9, 9});
  state({9, 9});
  f.emit(1, PushBackObjectRef{7, 3, 8});
  state({9, 9, 8});
  f.assign({9, 9, 8});
  state({9, 9, 8}); // Absolute same value after dirty deltas.
  f.assign({});
  state({});
  f.emit(1, PushBackObjectRef{7, 3, 42});
  state({42});
  f.dictionary(2);
  f.emit(2, BeginSetPropertyValue{7, 3, {1, 5}});
  f.emit(2, AppendPropertyValueData{f.bytes({80, 80}), 2});
  f.emit(1, PushBackObjectRef{7, 3, 43});
  state({42, 43});
  f.emit(2, EndSetPropertyValue{});
  state({80, 80});
  f.emit(2, CreatePropertyMessage{
                {6, 2},
                {6, 7},
                {{3, {1, 5}, 0, static_cast<std::uint32_t>(width)}},
                static_cast<std::uint32_t>(width)});
  f.emit(2, BeginPropertyMessageGroup{{6, 7}});
  f.emit(2, SendPropertyMessageFromGroup{7, f.bytes({90})});
  state({90});
  f.emit(1, PushBackObjectRef{7, 3, 91});
  state({90, 91});
  f.emit(2, EndPropertyMessageGroup{});
  live.append(f.events);
  live.finish();
  for (auto it = states.rbegin(); it != states.rend(); ++it)
    expected(live.snapshot_at_event(it->first), f, it->second);
  expected(before, f, {11, 22, 22});
  expected(live.latest(), f, {90, 91});
  // A rejected remove neither enters the history nor leaks a partial vector.
  Session failing(f.header, options);
  Fixture seed(order, width);
  seed.assign({4, 4, 5});
  failing.append(seed.events);
  auto prefix = failing.event_count();
  bool rejected = false;
  try {
    failing.append({Event{1, 0, 99, PushBackObjectRef{7, 3, 6}},
                    Event{1, 0, 100, RemoveObjectRef{7, 3, 999}}});
  } catch (const SessionError &) {
    rejected = true;
  }
  check(rejected && failing.failed() && failing.state_available() &&
            failing.event_count() == prefix + 1,
        "failed reference mutation corrupted accepted prefix");
  expected(failing.latest(), f, {4, 4, 5, 6});
  // Reused owners start clean, while an old published snapshot remains intact.
  Session reuse(seed.header);
  reuse.append(seed.events);
  auto old = reuse.latest();
  reuse.append({Event{1, 0, 0, DestroyInstance{7}},
                Event{1, 0, 0, CreateInstance{{6, 2}, 7}},
                Event{1, 0, 0, PushBackObjectRef{7, 3, 77}}});
  expected(old, seed, {4, 4, 5});
  expected(reuse.latest(), seed, {77});
  check(reuse.latest().instance(7)->generation == 1, "owner reuse generation");
}
std::uint64_t measured(std::size_t count) {
  Fixture f;
  f.events.reserve(f.events.size() + count);
  for (std::size_t i = 0; i < count; ++i)
    f.emit(1, PushBackObjectRef{7, 3, i % 7919});
  SessionOptions options;
  options.max_checkpoints = 0;
  allocations::bytes = 0;
  allocations::active = true;
  try {
    Session session(f.header, options);
    session.append(std::move(f.events));
    session.finish();
    allocations::active = false;
    auto used = allocations::bytes;
    const auto current = session.latest();
    check(current.instance(7)->property("Refs")->item_count == count,
          "large append count");
    check(current.instance(7)->property("Refs")->raw_bytes().size() ==
              count * 8,
          "large append canonical bytes");
    return used;
  } catch (...) {
    allocations::active = false;
    throw;
  }
}
void growth() {
#ifdef PVD_CORE_STATIC
  const auto small = measured(1000), large = measured(2000);
  check(large < small * 28 / 10 && large < 32u * 1024u * 1024u,
        "reference append allocation volume is quadratic");
  const auto real_scale = measured(40000);
  std::cout << "reference allocation bytes N1000=" << small
            << " N2000=" << large << " N40000=" << real_scale << '\n';
  // Debug STL iterator proxies and geometric capacity are part of this
  // allocator measurement too. Compare with the same build's small workload;
  // the preceding doubling check catches quadratic history before this run.
  check(real_scale < large * 40,
        "reference delta history exceeds linear allocation budget");
#else
  // An executable allocation override does not observe a Windows DLL's heap.
  measured(2000);
#endif
}
void budgets() {
  Fixture f;
  for (unsigned i = 0; i < 2048; ++i)
    f.emit(1, PushBackObjectRef{7, 3, i});
  SessionOptions options;
  options.checkpoint_interval = 32;
  options.max_checkpoints = 100;
  options.max_checkpoint_bytes = 32768;
  Session bounded(f.header, f.events, options);
  check(bounded.checkpoint_count() > 0 &&
            bounded.estimated_checkpoint_bytes() <=
                options.max_checkpoint_bytes,
        "checkpoint owned-structure budget exceeded");
  auto snapshot = bounded.snapshot_at_event(1000);
  check(snapshot.instance(7)->property("Refs")->item_count == 990,
        "evicted checkpoint reverse replay");
  options.max_checkpoint_bytes = 0;
  Session none(f.header, f.events, options);
  check(!none.checkpoint_count() && !none.estimated_checkpoint_bytes(),
        "zero checkpoint byte budget must disable checkpoint storage");
  options.max_checkpoint_bytes = 1;
  Session tiny(f.header, f.events, options);
  check(!tiny.checkpoint_count() && !tiny.estimated_checkpoint_bytes(),
        "oversize individual checkpoint must be skipped");
  options.max_checkpoint_bytes = std::numeric_limits<std::size_t>::max();
  options.max_checkpoints = std::numeric_limits<std::size_t>::max();
  Session unlimited(f.header, f.events, options);
  check(unlimited.checkpoint_count() > bounded.checkpoint_count(),
        "SIZE_MAX checkpoint budget must not overflow");
}
void allocation_failure_recovery() {
#if defined(PVD_CORE_STATIC) && (defined(__clang__) || defined(__GNUC__)) &&   \
    !(defined(_MSC_VER) && defined(_DEBUG))
  std::size_t failures = 0;
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    bool reached_success = false;
    for (std::size_t point = 1; point < 160; ++point) {
      Fixture f;
      f.assign({1});
      if (scenario == 1)
        f.emit(1, PushBackObjectRef{7, 3, 3});
      SessionOptions options;
      options.checkpoint_interval = scenario == 2 ? 1 : 0;
      options.max_checkpoints = 2;
      Session live(f.header, options);
      live.append(f.events);
      const auto prefix = live.event_count();
      const auto retained = live.latest();
      std::vector<Event> batch;
      if (scenario == 1)
        batch.push_back(
            {1, 0, 0, SetPropertyValue{7, 3, {1, 5}, 2, f.bytes({4, 5})}});
      else
        batch.push_back({1, 0, 0, PushBackObjectRef{7, 3, 2}});
      allocations::calls = 0;
      allocations::fail_at = point;
      allocations::fault = true;
      bool rejected = false;
      try {
        live.append(std::move(batch));
      } catch (const std::bad_alloc &) {
        rejected = true;
      } catch (...) {
        allocations::fault = false;
        throw;
      }
      allocations::fault = false;
      if (rejected) {
        ++failures;
        check(live.failed() && live.state_available(),
              "reference allocation failure did not recover prefix");
        check(live.event_count() == prefix || live.event_count() == prefix + 1,
              "reference allocation failure accepted invalid event prefix");
        const auto previous = scenario == 1 ? std::vector<std::uint64_t>{1, 3}
                                            : std::vector<std::uint64_t>{1};
        expected(retained, f, previous);
        expected(live.latest(), f,
                 live.event_count() == prefix
                     ? previous
                     : (scenario == 1 ? std::vector<std::uint64_t>{4, 5}
                                      : std::vector<std::uint64_t>{1, 2}));
        check(live.estimated_checkpoint_bytes() <= options.max_checkpoint_bytes,
              "recovered checkpoint budget inconsistent");
      } else {
        reached_success = true;
        break;
      }
    }
    check(reached_success, "reference fault allocation range incomplete");
  }
  check(failures > 10,
        "reference dirty/checkpoint failure paths were not exercised");
  std::cout << "reference allocation failure points verified=" << failures
            << '\n';
#endif
}
} // namespace
int main() {
  try {
    for (auto order : {ByteOrder::little, ByteOrder::big})
      for (unsigned width : {4u, 8u})
        for (bool checkpoints : {false, true})
          semantics(order, width, checkpoints);
    growth();
    budgets();
    allocation_failure_recovery();
    std::cout
        << "reference delta history/ABI/immutability/transactions/failure and "
           "checkpoint budgets passed\n";
    return 0;
  } catch (const std::exception &e) {
    allocations::active = false;
    allocations::fault = false;
    std::cerr << e.what() << '\n';
    return 1;
  }
}
