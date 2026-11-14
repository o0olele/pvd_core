#include "pvd/session.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

// Allocation replacement is a test mechanism, not a core API guarantee. It is
// exercised with the verified Clang/GCC static builds only. In particular, MSVC
// Debug replacement was observed to open a modal runtime-error dialog instead
// of propagating the catchable allocation fault this harness expects. A DLL
// also uses its own CRT allocation entry points. All normal/semantic-failure
// session tests still run on these configurations.
#if defined(PVD_CORE_STATIC) && (defined(__clang__) || defined(__GNUC__)) &&   \
    !(defined(_MSC_VER) && defined(_DEBUG))
#define PVD_SESSION_ALLOCATION_FAULT_TESTS 1
#else
#define PVD_SESSION_ALLOCATION_FAULT_TESTS 0
#endif
#if PVD_SESSION_ALLOCATION_FAULT_TESTS
namespace allocation_fault {
bool armed = false, persistent = false;
std::size_t calls = 0, fail_at = 0;
} // namespace allocation_fault
void *operator new(std::size_t size) {
  if (allocation_fault::armed &&
      ++allocation_fault::calls >= allocation_fault::fail_at &&
      (allocation_fault::persistent ||
       allocation_fault::calls == allocation_fault::fail_at))
    throw std::bad_alloc{};
  if (auto *memory = std::malloc(size ? size : 1))
    return memory;
  throw std::bad_alloc{};
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}
#endif

using namespace pvd;
namespace {
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
template <class F> void rejects(F f, const char *m) {
  try {
    f();
  } catch (const SessionError &) {
    return;
  }
  throw std::runtime_error(m);
}
void append_uint(Bytes &b, std::uint64_t v, std::size_t width,
                 ByteOrder order) {
  for (std::size_t i = 0; i < width; ++i) {
    auto shift = order == ByteOrder::little ? i : width - i - 1;
    b.push_back(static_cast<std::uint8_t>(v >> (shift * 8)));
  }
}
Bytes floats(std::initializer_list<float> vals,
             ByteOrder order = ByteOrder::little) {
  Bytes b;
  for (auto v : vals) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    append_uint(b, bits, 4, order);
  }
  return b;
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::uint64_t, std::map<std::string, std::uint32_t>> handles;
  explicit Fixture(ByteOrder order = ByteOrder::little, bool ptr64 = true) {
    header.byte_order = order;
    header.stream_flags = ptr64 ? 1 : 0;
  }
  template <class T> void emit(std::uint64_t s, T p) {
    events.push_back(
        {s, events.size() * 10, events.size() * 11, Payload{std::move(p)}});
  }
  std::uint32_t str(std::uint64_t s, const std::string &text) {
    if (text.empty())
      return 0;
    auto &hs = handles[s];
    auto it = hs.find(text);
    if (it != hs.end())
      return it->second;
    auto h = static_cast<std::uint32_t>(hs.size() + 1);
    hs.emplace(text, h);
    emit(s, StringHandleEvent{text, h});
    return h;
  }
  WireName name(std::uint64_t s, const char *text, const char *ns = "physx3") {
    auto a = str(s, ns), b = str(s, text);
    return {a, b};
  }
  void type(std::uint64_t s, const char *n) {
    auto wire = name(s, n, "test");
    emit(s, CreateClass{wire});
  }
  void property(std::uint64_t s, const char *c, const char *p, const char *dt,
                bool array = false, const char *semantic = "") {
    auto cls = name(s, c, "test");
    auto prop = str(s, p);
    auto sem = str(s, semantic);
    auto type = name(s, dt);
    emit(s, CreateProperty{cls,
                           prop,
                           sem,
                           type,
                           static_cast<std::uint8_t>(array ? 2 : 1),
                           {}});
  }
  void instance(std::uint64_t s, const char *c, std::uint64_t id) {
    auto n = name(s, c, "test");
    emit(s, CreateInstance{n, id});
  }
  void value(std::uint64_t s, std::uint64_t id, const char *p, const char *t,
             Bytes b, std::uint32_t count = 1) {
    auto h = str(s, p);
    auto n = name(s, t);
    emit(s, SetPropertyValue{id, h, n, count, std::move(b)});
  }
  void begin(std::uint64_t s, std::uint64_t scene) {
    auto h = str(s, "frame");
    emit(s, BeginSection{scene, h, events.size() * 10});
  }
  void end(std::uint64_t s, std::uint64_t scene) {
    auto h = str(s, "frame");
    emit(s, EndSection{scene, h, events.size() * 10});
  }
};
const PropertyValue &property(const Snapshot &s, std::uint64_t id,
                              const char *p) {
  auto o = s.instance(id);
  check(o, "missing live instance");
  auto v = o->property(p);
  check(v, "missing property");
  return *v;
}
void same_snapshot(const Snapshot &a, const Snapshot &b) {
  check(a.event_count == b.event_count &&
            a.instances.size() == b.instances.size(),
        "snapshot prefix/membership differs");
  for (const auto &[id, object] : a.instances) {
    const auto *other = b.instance(id);
    check(other && object.class_name == other->class_name &&
              object.generation == other->generation &&
              object.creating_stream == other->creating_stream &&
              object.pickable == other->pickable &&
              object.top_level == other->top_level &&
              object.properties.size() == other->properties.size(),
          "snapshot instance differs");
    check(object.color.has_value() == other->color.has_value(),
          "snapshot color presence");
    if (object.color)
      check(object.color->r == other->color->r &&
                object.color->g == other->color->g &&
                object.color->b == other->color->b &&
                object.color->a == other->color->a,
            "snapshot color differs");
    for (const auto &[name, value] : object.properties) {
      auto *v = other->property(name);
      check(v && value.incoming_type == v->incoming_type &&
                value.item_count == v->item_count &&
                value.byte_order == v->byte_order &&
                value.pointer_width == v->pointer_width &&
                value.raw_bytes() == v->raw_bytes() && value.text == v->text &&
                value.reference_ids == v->reference_ids &&
                value.opaque == v->opaque,
            "snapshot property differs");
    }
  }
  check(a.origin_shifts.size() == b.origin_shifts.size() &&
            a.cameras.size() == b.cameras.size(),
        "snapshot view metadata differs");
  for (const auto &[id, v] : a.origin_shifts) {
    const auto &o = b.origin_shifts.at(id);
    check(v.x == o.x && v.y == o.y && v.z == o.z,
          "snapshot origin shift differs");
  }
  for (const auto &[name, c] : a.cameras) {
    const auto &o = b.cameras.at(name);
    check(c.position.x == o.position.x && c.position.y == o.position.y &&
              c.position.z == o.position.z && c.up.x == o.up.x &&
              c.up.y == o.up.y && c.up.z == o.up.z &&
              c.target.x == o.target.x && c.target.y == o.target.y &&
              c.target.z == o.target.z,
          "snapshot camera differs");
  }
}
void check_live_every_event(const Fixture &fixture, const Session &offline) {
  SessionOptions options;
  options.checkpoint_interval = 5;
  options.max_checkpoints = 3;
  Session live(fixture.header, options);
  check(!live.finished() && !live.failed() && live.event_count() == 0 &&
            live.at_transaction_boundary(),
        "new live session state");
  std::vector<Snapshot> closed_history;
  std::map<std::uint64_t, bool> chunks, groups;
  for (std::size_t i = 0; i < fixture.events.size(); ++i) {
    const auto &event = fixture.events[i];
    if (std::holds_alternative<BeginSetPropertyValue>(event.payload))
      chunks[event.stream_id] = true;
    if (std::holds_alternative<EndSetPropertyValue>(event.payload))
      chunks.erase(event.stream_id);
    if (std::holds_alternative<BeginPropertyMessageGroup>(event.payload))
      groups[event.stream_id] = true;
    if (std::holds_alternative<EndPropertyMessageGroup>(event.payload))
      groups.erase(event.stream_id);
    live.append({event});
    check(live.event_count() == i + 1 && live.events().size() == i + 1,
          "one-event append valid prefix");
    check(live.at_transaction_boundary() == (chunks.empty() && groups.empty()),
          "cross-batch transaction boundary");
    check(!live.finished() && !live.failed() && live.checkpoint_count() <= 3,
          "live state/checkpoint budget");
    same_snapshot(live.latest(), offline.snapshot_at_event(i + 1));
    for (const auto &previous : closed_history)
      same_snapshot(previous, live.snapshot_at_event(previous.event_count));
    if (live.frames().size() > closed_history.size())
      closed_history.push_back(
          live.snapshot_at_frame(live.frames().size() - 1));
  }
  check(live.frames().size() == offline.frames().size() &&
            live.sections().size() == offline.sections().size() &&
            live.lifetimes().size() == offline.lifetimes().size() &&
            live.channels().size() == offline.channels().size(),
        "live indexes differ from offline load");
  live.finish();
  live.finish();
  check(live.finished() && !live.failed(),
        "finish is successful and idempotent");
  bool rejected = false;
  try {
    live.append({});
  } catch (const std::logic_error &) {
    rejected = true;
  }
  check(rejected && live.finished() && !live.failed(),
        "append after finish is rejected");
}
#if PVD_SESSION_ALLOCATION_FAULT_TESTS
void same_indexes(const Session &a, const Session &b) {
  check(a.event_count() == b.event_count() &&
            a.frames().size() == b.frames().size() &&
            a.sections().size() == b.sections().size() &&
            a.lifetimes().size() == b.lifetimes().size() &&
            a.channels().size() == b.channels().size() &&
            a.profile_zones().size() == b.profile_zones().size() &&
            a.checkpoint_count() == b.checkpoint_count() &&
            a.at_transaction_boundary() == b.at_transaction_boundary(),
        "recovered public index counts differ");
  const auto &ma = a.metadata();
  const auto &mb = b.metadata();
  check(ma.classes.size() == mb.classes.size() &&
            ma.messages.size() == mb.messages.size(),
        "recovered metadata counts differ");
  for (const auto &[name, type] : ma.classes) {
    const auto *other = mb.find_class(name);
    check(other && type.parent == other->parent &&
              type.properties.size() == other->properties.size(),
          "recovered class differs");
    for (const auto &[pname, property] : type.properties) {
      const auto *p = mb.find_property(name, pname);
      check(p && p->datatype == property.datatype &&
                p->property_type == property.property_type &&
                p->semantic == property.semantic &&
                p->named_values == property.named_values,
            "recovered property metadata differs");
    }
  }
  for (const auto &[name, message] : ma.messages) {
    auto found = mb.messages.find(name);
    check(found != mb.messages.end(), "missing recovered message");
    const auto &other = found->second;
    check(message.class_name == other.class_name &&
              message.byte_size == other.byte_size &&
              message.arguments.size() == other.arguments.size(),
          "recovered message differs");
    for (std::size_t i = 0; i < message.arguments.size(); ++i) {
      const auto &x = message.arguments[i];
      const auto &y = other.arguments[i];
      check(x.property_name == y.property_name && x.datatype == y.datatype &&
                x.offset == y.offset && x.byte_size == y.byte_size,
            "recovered message argument differs");
    }
  }
  for (std::size_t i = 0; i < a.lifetimes().size(); ++i) {
    const auto &x = a.lifetimes()[i];
    const auto &y = b.lifetimes()[i];
    check(x.id == y.id && x.generation == y.generation &&
              x.class_name == y.class_name &&
              x.creating_stream == y.creating_stream &&
              x.created_event == y.created_event &&
              x.destroyed_event == y.destroyed_event,
          "recovered lifetime differs");
  }
  for (std::size_t i = 0; i < a.frames().size(); ++i) {
    const auto &x = a.frames()[i];
    const auto &y = b.frames()[i];
    check(x.stream_id == y.stream_id && x.scene_id == y.scene_id &&
              x.index == y.index &&
              x.begin_event_count == y.begin_event_count &&
              x.event_count == y.event_count &&
              x.begin_timestamp == y.begin_timestamp &&
              x.end_timestamp == y.end_timestamp,
          "recovered frame differs");
  }
  for (std::size_t i = 0; i < a.sections().size(); ++i) {
    const auto &x = a.sections()[i];
    const auto &y = b.sections()[i];
    check(x.stream_id == y.stream_id && x.section_id == y.section_id &&
              x.name == y.name && x.begin_event_count == y.begin_event_count &&
              x.event_count == y.event_count &&
              x.begin_timestamp == y.begin_timestamp &&
              x.end_timestamp == y.end_timestamp,
          "recovered section differs");
  }
  for (std::size_t i = 0; i < a.channels().size(); ++i) {
    const auto &x = a.channels()[i];
    const auto &y = b.channels()[i];
    check(x.event_index == y.event_index && x.stream_id == y.stream_id &&
              x.instance_id == y.instance_id &&
              x.property_name == y.property_name && x.semantic == y.semantic &&
              x.value.raw_bytes() == y.value.raw_bytes(),
          "recovered raw channel differs");
  }
  for (const auto &[id, x] : a.profile_zones()) {
    const auto &y = b.profile_zones().at(id);
    check(x.id == y.id && x.name == y.name &&
              x.events.size() == y.events.size(),
          "recovered profile zone differs");
    for (const auto &[eid, event] : x.events) {
      const auto &o = y.events.at(eid);
      check(event.instance_id == o.instance_id &&
                event.event_id == o.event_id && event.name == o.name &&
                event.compile_time_enabled == o.compile_time_enabled,
            "recovered profile event differs");
    }
  }
  same_snapshot(a.latest(), b.latest());
  for (std::size_t n = a.event_count(); n-- > 0;)
    same_snapshot(a.snapshot_at_event(n), b.snapshot_at_event(n));
}
#endif
void allocation_failure_prefix_recovery() {
#if PVD_SESSION_ALLOCATION_FAULT_TESTS
  std::size_t injected_failures = 0;
  for (int scenario = 0; scenario < 10; ++scenario) {
    bool reached_success = false;
    for (std::size_t point = 1; point < 160; ++point) {
      Fixture f;
      f.type(1, "Actor");
      f.property(1, "Actor", "Mass", "PvdF32");
      f.property(1, "Actor", "Samples", "PvdF32", true);
      f.property(1, "Actor", "events", "PvdU8", true, "profile event stream");
      f.instance(1, "Actor", 1);
      f.instance(1, "Actor", 2);
      f.value(1, 1, "Mass", "PvdF32", floats({2}));
      f.begin(1, 55);
      auto actor = f.name(1, "Actor", "test"), number = f.name(1, "PvdF32");
      auto mass = f.str(1, "Mass"), samples = f.str(1, "Samples"),
           events = f.str(1, "events");
      auto child_name = f.str(1, "ChildClass");
      auto message = f.name(1, "MassMessage", "test");
      f.emit(1,
             CreatePropertyMessage{actor, message, {{mass, number, 0, 4}}, 4});
      Event target;
      switch (scenario) {
      case 0:
        target = {1, 0, 900, CreateInstance{actor, 99}};
        break;
      case 1:
        target = {1, 0, 901, DestroyInstance{1}};
        break;
      case 2:
        target = {1, 0, 902, EndSection{55, f.str(1, "frame"), 1000}};
        break;
      case 3:
        target = {1, 0, 903, SetPropertyValue{1, mass, number, 1, floats({5})}};
        break;
      case 4:
        target = {1, 0, 904, CreateClass{{f.str(1, "test"), child_name}}};
        break;
      case 5:
        target = {
            1, 0, 905,
            AddProfileZoneEvent{
                100, "long_profile_event_name_for_fault_testing", 2, true}};
        break;
      case 6:
        target = {
            1, 0, 906,
            SetPropertyValue{1, events, f.name(1, "PvdU8"), 3, Bytes{1, 2, 3}}};
        break;
      case 7:
        f.emit(1, BeginSetPropertyValue{1, samples, number});
        f.emit(1, AppendPropertyValueData{floats({1, 2}), 2});
        target = {1, 0, 907, EndSetPropertyValue{}};
        break;
      case 8:
        f.emit(1, BeginPropertyMessageGroup{message});
        f.emit(1, SendPropertyMessageFromGroup{1, floats({8})});
        target = {
            2, 0, 908,
            AddProfileZone{200, "long_profile_zone_name_for_fault_testing"}};
        break;
      default:
        f.emit(1, BeginSetPropertyValue{1, samples, number});
        f.emit(1, AppendPropertyValueData{floats({3}), 1});
        target = {
            2, 0, 909,
            AddProfileZone{200, "fault_during_open_chunk_on_another_stream"}};
        break;
      }
      SessionOptions options;
      options.checkpoint_interval = 1;
      options.max_checkpoints = 3;
      Session live(f.header, options);
      live.append(f.events);
      const auto old_count = live.event_count();
      std::vector<Event> batch{target};
      allocation_fault::calls = 0;
      allocation_fault::fail_at = point;
      allocation_fault::persistent = false;
      allocation_fault::armed = true;
      bool rejected = false;
      try {
        live.append(std::move(batch));
      } catch (const std::bad_alloc &) {
        rejected = true;
      } catch (...) {
        allocation_fault::armed = false;
        throw;
      }
      allocation_fault::armed = false;
      if (rejected) {
        ++injected_failures;
        check(live.failed() && !live.finished() && live.state_available(),
              "one-shot OOM did not recover state");
        check(live.event_count() >= old_count &&
                  live.event_count() <= old_count + 1,
              "OOM accepted invalid event count");
        Session reference(f.header, options);
        reference.append(live.events());
        same_indexes(live, reference);
      } else {
        check(live.event_count() == old_count + 1 && !live.failed(),
              "fault-free append did not complete");
        reached_success = true;
        break;
      }
    }
    check(reached_success,
          "allocation fault range did not cover append allocations");
  }
  check(injected_failures > 50,
        "too few allocation failure paths were exercised");
  std::cout << "allocation failure points verified: " << injected_failures
            << '\n';

  Fixture f;
  f.type(1, "Actor");
  Session persistent(f.header);
  persistent.append(f.events);
  auto actor = f.name(1, "Actor", "test");
  std::vector<Event> batch{Event{1, 0, 999, CreateInstance{actor, 99}}};
  const auto valid_prefix = persistent.event_count();
  allocation_fault::calls = 0;
  allocation_fault::fail_at = 1;
  allocation_fault::persistent = true;
  allocation_fault::armed = true;
  bool rejected = false, blocked = false;
  try {
    persistent.append(std::move(batch));
  } catch (const std::bad_alloc &) {
    rejected = true;
  }
  try {
    (void)persistent.frames();
  } catch (const SessionRecoveryError &) {
    blocked = true;
  }
  allocation_fault::armed = false;
  allocation_fault::persistent = false;
  check(rejected && blocked && persistent.failed() &&
            !persistent.state_available() &&
            persistent.event_count() == valid_prefix &&
            persistent.events().size() == valid_prefix &&
            !persistent.at_transaction_boundary(),
        "persistent OOM did not expose clear unavailable state");
  const auto require_unavailable = [](auto query) {
    bool caught = false;
    try {
      query();
    } catch (const SessionRecoveryError &) {
      caught = true;
    }
    check(caught, "query exposed partial state after failed recovery");
  };
  require_unavailable([&] { (void)persistent.metadata(); });
  require_unavailable([&] { (void)persistent.sections(); });
  require_unavailable([&] { (void)persistent.lifetimes(); });
  require_unavailable([&] { (void)persistent.channels(); });
  require_unavailable([&] { (void)persistent.profile_zones(); });
  require_unavailable([&] { (void)persistent.checkpoint_count(); });
  require_unavailable([&] { (void)persistent.latest(); });
  require_unavailable([&] { (void)persistent.snapshot_at_frame(0); });
  require_unavailable([&] { (void)persistent.snapshot_at_frame(1, 1, 0); });
  Session rebuilt(persistent.header());
  rebuilt.append(persistent.events());
  check(rebuilt.state_available() && rebuilt.event_count() == valid_prefix,
        "accepted source cannot be rebuilt after memory recovers");
#else
  std::cout << "controlled allocation injection skipped for this compiler/CRT; "
               "normal session tests ran\n";
#endif
}
void lifecycle_messages_and_reverse() {
  constexpr std::uint64_t a = 0x12345678abcdef01ull, b = 0xfedcba9876543210ull;
  Fixture f;
  f.type(10, "Base");
  f.type(10, "Actor");
  auto base = f.name(10, "Base", "test"), actor = f.name(10, "Actor", "test");
  f.emit(10, DeriveClass{base, actor});
  f.property(10, "Base", "Mass", "PvdF32");
  f.property(10, "Actor", "GlobalPose", "PxTransform");
  f.property(10, "Actor", "Name", "StringHandle");
  f.property(10, "Actor", "Samples", "PvdF32", true);
  f.property(10, "Actor", "Children", "ObjectRef", true);
  f.property(10, "Actor", "events", "PvdU8", true, "profile event stream");
  auto pose = f.str(10, "GlobalPose"), nm = f.str(10, "Name");
  auto transform = f.name(10, "PxTransform"), string = f.name(10, "String");
  auto msg = f.name(10, "ActorValues", "test"),
       posemsg = f.name(10, "PoseValues", "test");
  f.emit(10,
         CreatePropertyMessage{
             actor, msg, {{pose, transform, 0, 28}, {nm, string, 32, 8}}, 40});
  f.emit(10,
         CreatePropertyMessage{actor, posemsg, {{pose, transform, 0, 28}}, 28});
  f.instance(10, "Actor", a);
  f.instance(10, "Actor", b);
  auto before = f.events.size();
  f.begin(10, 42);
  Bytes blob = floats({0, 0, 0, 1, 1, 2, 3});
  blob.resize(40, 0);
  blob.insert(blob.end(), {'a', 'c', 't', 'o', 'r', 0});
  f.emit(10, SetPropertyMessage{a, msg, blob});
  f.value(10, a, "Mass", "PvdF32", floats({2.5f}));
  auto children = f.str(10, "Children");
  f.emit(10, PushBackObjectRef{a, children, b});
  f.emit(10, SetPickable{a, false});
  f.emit(10, SetIsTopLevel{a, true});
  f.emit(10, SetColor{a, {1, 2, 3, 4}});
  f.end(10, 42);
  auto first = f.events.size();
  f.begin(10, 42);
  f.emit(10, BeginPropertyMessageGroup{posemsg});
  f.emit(10, SendPropertyMessageFromGroup{a, floats({0, 0, 0, 1, 4, 5, 6})});
  f.emit(10, SendPropertyMessageFromGroup{b, floats({0, 0, 0, 1, 7, 8, 9})});
  f.emit(10, EndPropertyMessageGroup{});
  auto samples = f.str(10, "Samples");
  auto f32 = f.name(10, "PvdF32");
  f.emit(10, BeginSetPropertyValue{a, samples, f32});
  f.emit(10, AppendPropertyValueData{floats({1, 2}), 2});
  // A second stream has a different handle table, and a transaction in flight.
  auto other_samples = f.str(20, "Samples");
  auto other_f32 = f.name(20, "PvdF32");
  f.emit(20, BeginSetPropertyValue{b, other_samples, other_f32});
  f.emit(20, AppendPropertyValueData{floats({9}), 1});
  f.emit(10, AppendPropertyValueData{floats({3}), 1});
  f.emit(20, EndSetPropertyValue{});
  f.emit(10, EndSetPropertyValue{});
  f.emit(10, RemoveObjectRef{a, children, b});
  f.value(10, a, "events", "PvdU8", Bytes{1, 2, 3}, 3);
  f.end(10, 42);
  auto second = f.events.size();
  f.begin(10, 42);
  f.emit(10, DestroyInstance{b});
  f.end(10, 42);
  auto destroyed = f.events.size();
  f.begin(20, 42);
  f.instance(20, "Actor", b);
  f.value(20, b, "Mass", "PvdF32", floats({99}));
  f.end(20, 42);
  f.begin(10, 43);
  f.end(10, 43);
  auto custom_section = f.str(10, "validation_after_fetch");
  f.emit(10, BeginSection{a, custom_section, 1000});
  f.emit(10, EndSection{a, custom_section, 1100});
  f.emit(10, SetCamera{"test", {1, 2, 3}, {0, 1, 0}, {0, 0, 0}});
  f.emit(10, OriginShift{a, {1, 0, 0}});
  f.emit(10, OriginShift{a, {2, 0, 0}});
  SessionOptions options;
  options.checkpoint_interval = 7;
  options.max_checkpoints = 3;
  Session s(f.header, f.events, options);
  check(s.checkpoint_count() <= 3 && s.checkpoint_count() > 0,
        "checkpoint budget");
  check(s.frames().size() == 5, "multi-stream/scene frames");
  check(s.sections().size() == 6 &&
            s.sections().back().name == "validation_after_fetch",
        "non-frame section index");
  check(s.frames()[3].stream_id == 20 && s.frames()[3].index == 0,
        "per-stream frame ordinal");
  check(s.frames()[4].scene_id == 43 && s.frames()[4].index == 0,
        "per-scene frame ordinal");
  check(s.metadata().find_property({"test", "Actor"}, "Mass") != nullptr,
        "inherited reflection");
  check(s.metadata().is_a({"test", "Actor"}, {"test", "Base"}), "inheritance");
  check(s.lifetimes().size() == 3, "lifecycle count");
  check(s.lifetimes()[1].destroyed_event.has_value(), "destroyed lifetime");
  check(s.lifetimes()[2].generation == 1, "generation on ID reuse");
  auto state = s.snapshot_at_event(first);
  check(property(state, a, "Name").as_string() == "actor",
        "message pointer-string fixup");
  auto pose1 = property(state, a, "GlobalPose").as_transform();
  check(pose1 && pose1->position.x == 1 && pose1->position.z == 3 &&
            pose1->rotation.w == 1,
        "transform SDK memory layout");
  check(property(state, a, "Children").as_object_ref() == b,
        "64-bit object reference");
  check(!state.instance(a)->pickable && state.instance(a)->top_level &&
            state.instance(a)->color->a == 4,
        "object attributes");
  auto state2 = s.snapshot_at_event(second);
  check(property(state2, a, "GlobalPose").as_transform()->position.x == 4,
        "message group expansion");
  check(property(state2, a, "Samples").item_count == 3 &&
            property(state2, a, "Samples").as_float(2) == 3,
        "chunk assembly");
  check(property(state2, b, "Samples").as_float() == 9,
        "independent interleaved chunks");
  check(property(state2, a, "Children").reference_ids->empty(),
        "reference removal");
  check(s.channels().size() == 1 &&
            s.channels()[0].value.raw_bytes() == Bytes({1, 2, 3}),
        "raw channel retention");
  check(!s.snapshot_at_event(destroyed).instance(b),
        "destroy removed live object");
  check(s.latest().instance(b)->generation == 1, "reused object live");
  check(s.snapshot_at_event(before).instance(a)->properties.empty(),
        "reverse seek before assignment");
  check(s.latest().origin_shifts.at(a).x == 3,
        "origin shifts explicit cumulative offset");
  // Every event prefix must match a fresh replay without checkpoints, including
  // transactions.
  options.checkpoint_interval = 0;
  Session slow(f.header, f.events, options);
  check_live_every_event(f, slow);
  for (std::size_t n = f.events.size() + 1; n-- > 0;) {
    auto x = s.snapshot_at_event(n), y = slow.snapshot_at_event(n);
    check(x.instances.size() == y.instances.size(),
          "prefix checkpoint membership");
    for (const auto &obj : x.instances) {
      auto o = y.instance(obj.first);
      check(o && o->generation == obj.second.generation &&
                o->properties.size() == obj.second.properties.size(),
            "prefix checkpoint generation/properties");
      for (const auto &v : obj.second.properties)
        check(o->property(v.first)->raw_bytes() == v.second.raw_bytes(),
              "prefix checkpoint bytes");
    }
  }
}
void endian_pointer_width_and_failures() {
  Fixture f(ByteOrder::big, false);
  f.type(1, "Actor");
  f.property(1, "Actor", "Point", "PxVec3");
  f.property(1, "Actor", "Ref", "ObjectRef");
  f.property(1, "Actor", "Name", "StringHandle");
  f.instance(1, "Actor", 1);
  f.value(1, 1, "Point", "PxVec3", floats({-2, 3, 4}, ByteOrder::big));
  Bytes ref;
  append_uint(ref, 0xf1234567u, 4, ByteOrder::big);
  f.value(1, 1, "Ref", "VoidPtr", ref);
  auto name_id = f.str(1, "big-endian");
  Bytes handle;
  append_uint(handle, name_id, 4, ByteOrder::big);
  f.value(1, 1, "Name", "StringHandle", handle);
  SessionOptions huge;
  huge.checkpoint_interval = 1;
  huge.max_checkpoints = std::numeric_limits<std::size_t>::max();
  Session s(f.header, f.events, huge);
  auto state = s.latest();
  check(property(state, 1, "Point").as_vec3()->x == -2, "big-endian float");
  check(property(state, 1, "Ref").as_object_ref() == 0xf1234567u,
        "32-bit source pointer");
  check(property(state, 1, "Name").as_string() == "big-endian",
        "StringHandle resolution");
  auto bad = f.events;
  bad.push_back({1, 0, 100, EndSetPropertyValue{}});
  rejects([&] { Session x(f.header, bad); }, "reject stray transaction end");
  bad = f.events;
  bad.push_back({2, 0, 101, CreateInstance{{900, 901}, 2}});
  rejects([&] { Session x(f.header, bad); }, "reject missing stream handles");
  auto point = f.str(1, "Point");
  auto vec = f.name(1, "PxVec3");
  bad = f.events;
  bad.push_back({1, 0, 102, SetPropertyValue{1, point, vec, 1, Bytes{0}}});
  rejects([&] { Session x(f.header, bad); }, "reject malformed value");
  bad = f.events;
  bad.push_back({1, 0, 103, BeginSetPropertyValue{1, point, vec}});
  rejects([&] { Session x(f.header, bad); }, "reject unfinished transaction");
  bad = f.events;
  bad.push_back({1, 0, 104, RemoveObjectRef{1, f.str(1, "Ref"), 123}});
  rejects([&] { Session x(f.header, bad); },
          "reject scalar reference array mutation");
  auto cycle = f.events;
  auto actor = f.name(1, "Actor", "test");
  cycle.push_back({1, 0, 105, DeriveClass{actor, actor}});
  rejects([&] { Session x(f.header, cycle); }, "reject inheritance cycle");
}
void incremental_failure_prefix_and_finish() {
  Fixture f;
  f.type(1, "Actor");
  f.property(1, "Actor", "Mass", "PvdF32");
  f.property(1, "Actor", "Name", "StringHandle");
  f.property(1, "Actor", "events", "PvdU8", true, "profile event stream");
  f.property(1, "Actor", "Samples", "PvdF32", true);
  f.instance(1, "Actor", 1);
  auto type = f.name(1, "Actor", "test"), msg = f.name(1, "BadMessage", "test");
  auto events = f.str(1, "events"), name = f.str(1, "Name");
  auto byte = f.name(1, "PvdU8"), string = f.name(1, "String");
  f.emit(1, CreatePropertyMessage{
                type, msg, {{events, byte, 0, 1}, {name, string, 8, 8}}, 16});
  f.value(1, 1, "Mass", "PvdF32", floats({2}));
  auto mass = f.str(1, "Mass"), samples = f.str(1, "Samples");
  auto number = f.name(1, "PvdF32");
  Session live(f.header);
  live.append(f.events);
  const auto previous = live.event_count();
  Event good{1, 0, 500, SetPropertyValue{1, mass, number, 1, floats({7})}};
  // First message argument is a valid raw channel, but the later string fixup
  // is missing. Neither argument nor the channel may leak into the valid
  // prefix.
  Event bad{1, 0, 501, SetPropertyMessage{1, msg, Bytes(16, 0)}};
  rejects([&] { live.append({good, bad}); },
          "reject bad message in append batch");
  check(live.failed() && !live.finished() && live.event_count() == previous + 1,
        "failed append retains only successful events");
  check(property(live.latest(), 1, "Mass").as_float() == 7 &&
            live.channels().empty() &&
            !live.latest().instance(1)->property("events") &&
            !live.latest().instance(1)->property("Name"),
        "rejected message changed object/channel state");
  check(property(live.snapshot_at_event(previous), 1, "Mass").as_float() == 2,
        "failed append changed old history");
  bool rejected = false;
  try {
    live.append({good});
  } catch (const std::logic_error &) {
    rejected = true;
  }
  check(rejected && live.event_count() == previous + 1,
        "failed session rejects later append");

  Session unfinished(f.header);
  unfinished.append(f.events);
  unfinished.append(
      {Event{1, 0, 600, BeginSetPropertyValue{1, samples, number}}});
  unfinished.append(
      {Event{1, 0, 601, AppendPropertyValueData{floats({3}), 1}}});
  const auto before_finish = unfinished.event_count();
  rejects([&] { unfinished.finish(); },
          "finish rejects pending property chunk");
  check(unfinished.failed() && !unfinished.finished() &&
            !unfinished.at_transaction_boundary() &&
            unfinished.event_count() == before_finish &&
            !unfinished.latest().instance(1)->property("Samples"),
        "finish error lost successful prefix or published unfinished property");

  Session invalid_end(f.header);
  invalid_end.append(f.events);
  invalid_end.append(
      {Event{1, 0, 602, BeginSetPropertyValue{1, samples, number}}});
  invalid_end.append({Event{1, 0, 603, AppendPropertyValueData{Bytes{0}, 1}}});
  const auto prefix = invalid_end.event_count();
  rejects(
      [&] { invalid_end.append({Event{1, 0, 604, EndSetPropertyValue{}}}); },
      "malformed final chunk rejected");
  check(invalid_end.failed() && invalid_end.event_count() == prefix &&
            !invalid_end.at_transaction_boundary() &&
            !invalid_end.latest().instance(1)->property("Samples"),
        "rejected chunk end altered pending transaction or state");

  auto group_message = f.name(1, "MassMessage", "test");
  f.emit(1,
         CreatePropertyMessage{type, group_message, {{mass, number, 0, 4}}, 4});
  Session grouped(f.header);
  grouped.append(f.events);
  grouped.append({Event{1, 0, 700, BeginPropertyMessageGroup{group_message}}});
  grouped.append(
      {Event{1, 0, 701, SendPropertyMessageFromGroup{1, floats({9})}}});
  check(!grouped.at_transaction_boundary() &&
            property(grouped.latest(), 1, "Mass").as_float() == 9,
        "in-flight group publishes complete messages individually");
  const auto group_prefix = grouped.event_count();
  rejects([&] { grouped.finish(); }, "finish rejects unfinished message group");
  check(grouped.failed() && grouped.event_count() == group_prefix &&
            property(grouped.latest(), 1, "Mass").as_float() == 9,
        "group finish failure lost its accepted message prefix");

  Session metadata_failure(f.header);
  metadata_failure.append(f.events);
  const auto metadata_prefix = metadata_failure.event_count();
  auto contradictory_type = f.name(1, "PvdU8");
  Event conflicting{1, 0, 702,
                    CreateProperty{type, mass, 0, contradictory_type, 1, {}}};
  rejects([&] { metadata_failure.append({conflicting}); },
          "conflicting metadata rejected");
  const auto *definition =
      metadata_failure.metadata().find_property({"test", "Actor"}, "Mass");
  check(metadata_failure.failed() &&
            metadata_failure.event_count() == metadata_prefix && definition &&
            definition->datatype == QualifiedName{"physx3", "PvdF32"},
        "rejected metadata changed accepted reflection schema");

  // Ordinary open sections do not prevent recoverable recording boundaries.
  Fixture section;
  section.begin(9, 123);
  Session open(section.header);
  open.append(section.events);
  check(open.at_transaction_boundary(),
        "open frame must allow recording boundary");
  open.finish();
  check(open.finished(), "finish permits an open ordinary section");
}
void incremental_long_multiscene_history() {
  Fixture f;
  f.type(1, "Actor");
  f.property(1, "Actor", "Mass", "PvdF32");
  f.instance(1, "Actor", 99);
  SessionOptions options;
  options.checkpoint_interval = 11;
  options.max_checkpoints = 4;
  Session live(f.header, options);
  live.append(f.events);
  std::size_t cursor = f.events.size();
  std::vector<std::pair<std::size_t, float>> saved;
  for (std::size_t round = 0; round < 500; ++round) {
    auto stream = std::uint64_t(round % 3 + 1),
         scene = std::uint64_t(round % 5 + 10);
    f.begin(stream, scene);
    f.value(stream, 99, "Mass", "PvdF32", floats({static_cast<float>(round)}));
    f.end(stream, scene);
    live.append(std::vector<Event>(f.events.begin() + cursor, f.events.end()));
    cursor = f.events.size();
    check(live.frames().size() == round + 1 && live.checkpoint_count() <= 4,
          "long live frame index/checkpoint budget");
    saved.emplace_back(live.event_count(), static_cast<float>(round));
    const auto &old = saved[round / 2];
    check(property(live.snapshot_at_event(old.first), 99, "Mass").as_float() ==
              old.second,
          "append changed long-lived event history");
  }
  live.finish();
  Session offline(f.header, f.events, options);
  for (std::size_t frame = live.frames().size(); frame-- > 0;) {
    const auto &marker = live.frames()[frame];
    check(marker.stream_id == offline.frames()[frame].stream_id &&
              marker.scene_id == offline.frames()[frame].scene_id &&
              marker.index == offline.frames()[frame].index,
          "long multi-scene frame ordinal differs");
    same_snapshot(live.snapshot_at_frame(frame),
                  offline.snapshot_at_frame(frame));
  }
}
void repeated_messages_preserve_atomic_state_and_packets() {
  Fixture f;
  f.type(1, "Repeated");
  f.property(1, "Repeated", "value", "PvdF32");
  f.property(1, "Repeated", "packet", "PvdU8", true, "profile event stream");
  f.instance(1, "Repeated", 7);
  f.value(1, 7, "value", "PvdF32", floats({0.f}));
  f.value(1, 7, "packet", "PvdU8", {1, 2}, 2);
  const auto initial = f.events.size();
  auto cls = f.name(1, "Repeated", "test"),
       msg = f.name(1, "RepeatedMessage", "test"), dt = f.name(1, "PvdF32");
  auto value = f.str(1, "value");
  f.emit(1, CreatePropertyMessage{cls, msg, {{value, dt, 0, 4}, {value, dt, 4, 4}}, 8});
  f.emit(1, SetPropertyMessage{7, msg, floats({1.f, 0.f})});
  const auto restored = f.events.size();
  f.value(1, 7, "packet", "PvdU8", {1, 2}, 2);
  f.value(1, 7, "packet", "PvdU8", {1, 2}, 2);
  f.value(1, 7, "value", "PvdF32", floats({-0.f}));
  const auto minus_zero = f.events.size();
  Bytes zeros(4);
  f.value(1, 7, "value", "PvdU32", zeros);
  const auto changed_type = f.events.size();
  f.emit(1, SetPropertyMessage{7, msg, floats({0.f, 2.f})});
  SessionOptions options;
  options.checkpoint_interval = 3;
  Session session(f.header, f.events, options);
  check(session.events().size() == f.events.size() && session.channels().size() == 3,
        "repeated channel packets/raw events were coalesced");
  for (const auto &packet : session.channels())
    check(packet.value.raw_bytes() == Bytes({1, 2}), "channel bytes changed");
  auto original = session.snapshot_at_event(initial);
  check(property(session.snapshot_at_event(restored), 7, "value").as_float() == 0.f,
        "atomic message last repeated assignment must restore old state");
  auto zero = property(session.snapshot_at_event(minus_zero), 7, "value").as_float();
  check(zero && std::signbit(*zero), "negative-zero wire change was lost");
  check(property(session.snapshot_at_event(changed_type), 7, "value").incoming_type.name == "PvdU32",
        "equal bytes with different incoming type were merged");
  check(property(session.latest(), 7, "value").as_float() == 2.f,
        "atomic message final changed value was lost");
  check(property(original, 7, "value").as_float() == 0.f,
        "retained snapshot changed");
  for (std::size_t i = f.events.size(); i-- > initial;)
    check(session.snapshot_at_event(i).event_count == i, "no-op source prefix lost");
}
} // namespace
int main() {
  try {
    lifecycle_messages_and_reverse();
    endian_pointer_width_and_failures();
    incremental_failure_prefix_and_finish();
    incremental_long_multiscene_history();
    repeated_messages_preserve_atomic_state_and_packets();
    allocation_failure_prefix_recovery();
    std::cout << "session tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
