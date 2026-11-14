#include "live_session.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
using namespace pvd;
using namespace pvd::viewer;
using namespace std::chrono_literals;
namespace {
void need(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
Bytes floats(std::initializer_list<float> items) {
  Bytes b;
  for (float n : items) {
    std::uint32_t v;
    std::memcpy(&v, &n, 4);
    for (int i = 0; i < 4; ++i)
      b.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
  }
  return b;
}
Bytes pointers(std::initializer_list<std::uint64_t> items) {
  Bytes b;
  for (auto v : items)
    for (int i = 0; i < 8; ++i)
      b.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
  return b;
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  Fixture() { header.stream_flags = 1; }
  template <class T> void emit(T payload) {
    events.push_back(
        {1, events.size(), events.size() * 8, Payload{std::move(payload)}});
  }
  std::uint32_t string(const std::string &value) {
    if (value.empty())
      return 0;
    auto found = handles.find(value);
    if (found != handles.end())
      return found->second;
    auto id = static_cast<std::uint32_t>(handles.size() + 1);
    handles.emplace(value, id);
    emit(StringHandleEvent{value, id});
    return id;
  }
  WireName type(const char *n) {
    auto ns = string("physx3"), name = string(n);
    return {ns, name};
  }
  void property(const char *c, const char *n, const char *t,
                bool array = false) {
    auto cls = type(c), dtype = type(t);
    auto name = string(n);
    emit(CreateProperty{
        cls, name, 0, dtype, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void instance(const char *c, std::uint64_t id) {
    emit(CreateInstance{type(c), id});
  }
  void value(std::uint64_t id, const char *p, const char *t, Bytes data,
             std::uint32_t count = 1) {
    auto name = string(p);
    auto dtype = type(t);
    emit(SetPropertyValue{id, name, dtype, count, std::move(data)});
  }
  void ref(std::uint64_t id, const char *p,
           std::initializer_list<std::uint64_t> ids) {
    value(id, p, "VoidPtr", pointers(ids),
          static_cast<std::uint32_t>(ids.size()));
  }
  void pose(std::uint64_t id, float x) {
    value(id, "GlobalPose", "PxTransform", floats({0, 0, 0, 1, x, 0, 0}));
  }
  void section(std::uint64_t scene, bool begin, const char *label = "frame") {
    auto h = string(label);
    if (begin)
      emit(BeginSection{scene, h, events.size()});
    else
      emit(EndSection{scene, h, events.size()});
  }
  void setup() {
    for (const char *n :
         {"PxScene", "PxRigidStatic", "PxShape", "PxBoxGeometry"})
      emit(CreateClass{type(n)});
    property("PxScene", "Samples", "PvdF32", true);
    property("PxRigidStatic", "Scene", "ObjectRef");
    property("PxRigidStatic", "Shapes", "ObjectRef", true);
    property("PxRigidStatic", "GlobalPose", "PxTransform");
    property("PxShape", "Geometry", "ObjectRef");
    property("PxShape", "LocalPose", "PxTransform");
    property("PxShape", "Flags", "PvdU8");
    property("PxBoxGeometry", "HalfExtents", "PxVec3");
    instance("PxScene", 1);
    instance("PxScene", 2);
    instance("PxBoxGeometry", 200);
    value(200, "HalfExtents", "PxVec3", floats({.5f, .5f, .5f}));
    instance("PxShape", 100);
    ref(100, "Geometry", {200});
    value(100, "LocalPose", "PxTransform", floats({0, 0, 0, 1, 0, 0, 0}));
    value(100, "Flags", "PvdU8", Bytes{11});
    actor(10, 1);
    actor(20, 2);
  }
  void actor(std::uint64_t id, std::uint64_t scene) {
    instance("PxRigidStatic", id);
    ref(id, "Scene", {scene});
    ref(id, "Shapes", {100});
    pose(id, 0);
  }
};
void incremental_model() {
  Fixture f;
  f.setup();
  Document document;
  document.begin_live(f.header, "synthetic live");
  need(document.live() && document.follow_live(),
       "live model starts following");
  document.append_live(f.events);
  std::size_t cursor = f.events.size();
  auto append = [&] {
    std::vector<Event> delta(f.events.begin() + cursor, f.events.end());
    cursor = f.events.size();
    return document.append_live(std::move(delta));
  };
  f.section(1, true);
  f.pose(10, 1);
  f.section(1, false);
  append();
  need(document.timeline().size() == 1 && document.frame()->draws.size() == 1,
       "first live frame published");
  auto first = document.frame();
  auto mesh = first->draws[0].mesh;
  auto key = first->draws[0].key;
  need(document.select(key), "select live shape");
  document.set_frame(0);
  auto paused = document.frame();
  need(!document.follow_live(), "manual seek pauses follow");
  for (int i = 2; i <= 40; ++i) {
    f.section(1, true);
    f.pose(10, float(i));
    f.section(1, false);
    f.section(2, true);
    f.pose(20, float(i * 2));
    f.section(2, false);
    append();
  }
  need(document.frame() == paused && document.timeline().size() == 40 &&
           document.selected_key() == key,
       "paused history stays immutable while frame indexes grow");
  need(document.scenes().size() == 2 &&
           document.scenes()[1].native_frames == 39,
       "second scene grows incrementally");
  document.set_follow_live(true);
  need(document.frame_index() == 39 && document.frame()->draws[0].mesh == mesh,
       "follow resumes latest using immutable mesh cache");
  need(document.frame()->render.instances[0].world_pose.position.x == 40,
       "follow displays newest native marker");
  // A bounded transport batch may cut a transaction. Its property is never
  // published as latest/EOF, including scene switches without completed frames.
  auto samples = f.string("Samples");
  auto dtype = f.type("PvdF32");
  f.emit(BeginSetPropertyValue{1, samples, dtype});
  append();
  auto boundary_frame = document.frame();
  f.emit(AppendPropertyValueData{floats({1, 2}), 2});
  append();
  need(document.frame() == boundary_frame &&
           !document.session()->at_transaction_boundary(),
       "partial chunk retains published frame");
  f.emit(EndSetPropertyValue{});
  f.section(1, true);
  f.pose(10, 41);
  f.section(1, false);
  append();
  need(document.frame_index() == 40 &&
           document.session()->at_transaction_boundary(),
       "closed chunk and new frame publish together");
  need(first->snapshot.instance(10)
                   ->property("GlobalPose")
                   ->as_transform()
                   ->position.x == 1 &&
           first->draws[0].mesh == mesh,
       "old published frame survived ingestion");
  document.set_frame(0);
  auto historical = document.frame();
  f.section(1, true);
  f.pose(10, 42);
  f.section(1, false);
  append();
  document.finish_live();
  need(!document.live() && document.frame() == historical &&
           document.timeline().size() == 42,
       "finish preserves paused history");
  need(document.session()->finished(),
       "live Session finishes without reparsing source");
  Document alternate;
  alternate.begin_live(f.header);
  alternate.append_live(f.events);
  alternate.set_follow_live(false);
  auto before = alternate.frame();
  Fixture extra;
  extra.header = f.header;
  // Per-event import is equivalent to batch import, including all prefixes.
  Document one;
  one.begin_live(f.header);
  for (const auto &event : f.events)
    one.append_live({event});
  need(one.live_event_count() == document.live_event_count() &&
           one.timeline().size() == 42,
       "one-event batches do not lose timeline indexes");
  need(one.frame()->render.instances[0].world_pose.position.x == 42,
       "one-event follow reaches same state");
  one.finish_live();
}
void idle_controller_and_poll() {
  TcpCapture capture(CaptureOptions{});
  auto empty = capture.poll_from(0, 1, 1);
  need(!empty.header && empty.events.empty() && empty.caught_up(),
       "idle atomic poll");
  bool rejected = false;
  try {
    capture.poll_from(1);
  } catch (const std::out_of_range &) {
    rejected = true;
  }
  need(rejected, "future cursor rejected");
  Document doc;
  LiveSession controller;
  CaptureOptions options;
  options.port = 0;
  options.poll_interval = 5ms;
  controller.start(options);
  need(controller.active() &&
           controller.status().state == CaptureState::listening &&
           controller.status().port,
       "listener is available without blocking for a client");
  controller.poll(doc);
  need(!doc.loaded(), "old document is retained before first stream header");
  controller.stop();
  auto end = std::chrono::steady_clock::now() + 2s;
  while (controller.active() && std::chrono::steady_clock::now() < end) {
    controller.poll(doc);
    std::this_thread::sleep_for(5ms);
  }
  need(!controller.active() &&
           controller.status().state == CaptureState::cancelled &&
           controller.caught_up(),
       "cancel idle listener and retain coherent status");
}
std::string shell_quote(const std::string &text) {
#ifdef _WIN32
  if (text.find_first_of("\"%!\r\n") != std::string::npos)
    throw std::invalid_argument(
        "test process path has unsupported cmd expansion characters");
  return "\"" + text + "\"";
#else
  std::string result = "'";
  for (char c : text)
    result += c == '\'' ? "'\"'\"'" : std::string(1, c);
  return result + "'";
#endif
}
std::future<int> producer(const std::string &exe,
                          const std::filesystem::path &prefix,
                          std::uint16_t port, unsigned steps, unsigned pause) {
  auto command = shell_quote(exe) + " --tcp 127.0.0.1 " + std::to_string(port) +
                 " " + shell_quote(prefix.u8string()) + " " +
                 std::to_string(steps) + " " + std::to_string(pause);
#ifdef _WIN32
  command = "\"" + command + "\"";
#endif
  return std::async(std::launch::async,
                    [command] { return std::system(command.c_str()); });
}
void same_snapshot(const Snapshot &a, const Snapshot &b) {
  need(a.event_count == b.event_count &&
           a.instances.size() == b.instances.size(),
       "recorded/live snapshot membership");
  for (const auto &[id, object] : a.instances) {
    auto other = b.instance(id);
    need(other && other->generation == object.generation &&
             other->class_name == object.class_name &&
             other->properties.size() == object.properties.size(),
         "recorded/live object identity");
    for (const auto &[name, value] : object.properties) {
      auto v = other->property(name);
      need(v && v->raw_bytes() == value.raw_bytes() &&
               v->incoming_type == value.incoming_type &&
               v->reference_ids == value.reference_ids && v->text == value.text,
           "recorded/live property data");
    }
  }
}
void real_controller(const std::string &exe,
                     const std::filesystem::path &root) {
  std::filesystem::create_directories(root);
  auto recording =
      root / std::filesystem::u8path(u8"\u5b9e\u65f6\u5f55\u5236.pxd2");
  LiveSession controller({2048, 1024 * 1024});
  Document doc;
  CaptureOptions options;
  options.port = 0;
  options.poll_interval = 10ms;
  options.recording_path = recording.u8string();
  controller.start(options);
  auto process =
      producer(exe, root / "controller", controller.status().port, 160, 10);
  auto deadline = std::chrono::steady_clock::now() + 15s;
  std::size_t receiving_updates = 0;
  bool paused = false, resumed = false;
  std::shared_ptr<const FrameState> held;
  std::size_t old_timeline = 0;
  std::uint64_t last_bytes = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    controller.poll(doc);
    need(controller.error().empty(), "controller import error");
    need(controller.status().wire_bytes >= last_bytes &&
             controller.accepted_event_count() <=
                 controller.status().recoverable_event_count,
         "coherent live status and cursor");
    last_bytes = controller.status().wire_bytes;
    if (controller.status().state == CaptureState::receiving &&
        doc.timeline().size() > 1) {
      ++receiving_updates;
      if (!paused && doc.timeline().size() > 8) {
        doc.set_frame(1);
        held = doc.frame();
        old_timeline = doc.timeline().size();
        paused = true;
      } else if (paused && !resumed &&
                 doc.timeline().size() > old_timeline + 8) {
        need(doc.frame() == held && !doc.follow_live(),
             "receive did not preserve paused historical viewport");
        controller.set_follow_live(true);
        resumed = true;
      }
    }
    if (!controller.active() && controller.status().terminal())
      break;
    std::this_thread::sleep_for(100ms);
  }
  need(process.wait_for(2s) == std::future_status::ready,
       "SDK producer did not finish");
  need(process.get() == 0, "SDK producer failed");
  // Drain any deliberately bounded tail after transport EOF.
  for (int i = 0; controller.active() && i < 100; ++i)
    controller.poll(doc);
  need(controller.status().state == CaptureState::completed &&
           !controller.active() && controller.caught_up() &&
           controller.error().empty(),
       "live controller did not complete safe prefix");
  need(paused && resumed && receiving_updates >= 3,
       "real ingestion did not expose responsive intermediate updates");
  need(doc.scenes().size() == 2 && doc.timeline().size() == 160 &&
           !doc.live() && doc.session()->finished(),
       "two-scene live model completion");
  Document saved_document;
  saved_document.open_file(recording.u8string());
  const auto &saved = *saved_document.session();
  need(saved.event_count() == controller.accepted_event_count() &&
           saved.event_count() == doc.live_event_count(),
       "Unicode recording and accepted delta count differ");
  need(std::filesystem::file_size(recording) ==
           controller.status().recorded_bytes,
       "Unicode recording byte count");
  for (auto index : {std::size_t(0), std::size_t(79), std::size_t(159)}) {
    const auto &entry = doc.timeline()[index];
    same_snapshot(saved.snapshot_at_event(entry.event_count),
                  doc.session()->snapshot_at_event(entry.event_count));
  }
  need(held && held->snapshot.event_count < doc.frame()->snapshot.event_count,
       "published historical frame survived live tail");
}
void bounded_transport(const std::string &exe,
                       const std::filesystem::path &root) {
  CaptureOptions options;
  options.port = 0;
  options.poll_interval = 5ms;
  options.recording_path = (root / "bounded.pxd2").u8string();
  TcpCapture capture(options);
  capture.start();
  auto process =
      producer(exe, root / "bounded_expected", capture.status().port, 40, 1);
  std::size_t cursor = 0, copies = 0, polls = 0, data_polls = 0,
              empty_polls = 0;
  double poll_seconds = 0, append_seconds = 0;
  const auto started = std::chrono::steady_clock::now();
  std::unique_ptr<Session> imported;
  const auto receive_deadline = std::chrono::steady_clock::now() + 15s;
  std::optional<std::chrono::steady_clock::time_point> terminal_seen;
  auto last_progress = std::chrono::steady_clock::now();
  bool deadline_expired = false;
  while (true) {
    auto call_begin = std::chrono::steady_clock::now();
    auto delta = capture.poll_from(cursor, 3, 256);
    poll_seconds += std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - call_begin)
                        .count();
    ++polls;
    need(delta.begin_event_count == cursor &&
             delta.end_event_count - cursor == delta.events.size() &&
             delta.end_event_count <= delta.status.recoverable_event_count &&
             delta.events.size() <= 3,
         "bounded atomic cursor slice");
    if (delta.copied_payload_bytes > 256)
      need(delta.events.size() == 1 && delta.oversized_event,
           "byte budget exceeded by more than one atomic event");
    if (delta.header && !imported)
      imported = std::make_unique<Session>(*delta.header);
    const bool had_events = !delta.events.empty();
    if (had_events)
      ++data_polls;
    else
      ++empty_polls;
    call_begin = std::chrono::steady_clock::now();
    if (imported && had_events)
      imported->append(std::move(delta.events));
    append_seconds += std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - call_begin)
                          .count();
    copies += delta.end_event_count - cursor;
    if (delta.end_event_count != cursor)
      last_progress = std::chrono::steady_clock::now();
    cursor = delta.end_event_count;
    const auto now = std::chrono::steady_clock::now();
    if (delta.status.terminal()) {
      if (!terminal_seen)
        terminal_seen = now;
      if (delta.caught_up())
        break;
      // EOF and consuming its already-safe tail are different phases. Tiny
      // batches deliberately stress cursor correctness; permit forward progress
      // after EOF, but diagnose a stalled or unreasonably long drain
      // separately.
      if (now - last_progress > 5s || now - *terminal_seen > 20s) {
        deadline_expired = true;
        break;
      }
    } else if (now > receive_deadline) {
      deadline_expired = true;
      break;
    }
    if (!had_events)
      std::this_thread::sleep_for(1ms);
  }
  if (!capture.status().terminal())
    capture.cancel();
  capture.wait();
  need(process.wait_for(2s) == std::future_status::ready && process.get() == 0,
       "bounded producer failed");
  auto status = capture.status();
  std::cout << "bounded diagnostics: state=" << capture_state_name(status.state)
            << " cursor=" << cursor
            << " safe=" << status.recoverable_event_count
            << " accepted=" << status.event_count << " copies=" << copies
            << " polls=" << polls << " data=" << data_polls
            << " empty=" << empty_polls << " poll_s=" << poll_seconds
            << " append_s=" << append_seconds
            << " deadline_expired=" << deadline_expired << " wall_s="
            << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             started)
                   .count()
            << '\n';
  need(status.state == CaptureState::completed &&
           cursor == status.recoverable_event_count && copies == cursor &&
           imported,
       "bounded event delta did not finish exactly once");
  imported->finish();
  auto empty = capture.poll_from(cursor);
  need(empty.events.empty() && empty.caught_up() && empty.status.terminal(),
       "caught-up poll recopied historical events");
  auto saved = Session::load_file(options.recording_path);
  same_snapshot(saved.latest(), imported->latest());
  need(saved.frames().size() == 80 && imported->frames().size() == 80,
       "two-scene frame indexes survive event-sized polls");
}
} // namespace
int main(int argc, char **argv) {
  try {
    incremental_model();
    idle_controller_and_poll();
    if (argc > 1) {
      auto base = argc > 2 ? std::filesystem::u8path(argv[2])
                           : std::filesystem::path("build/viewer-live-tests");
      auto unique = std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
      auto directory = base / unique;
      std::filesystem::create_directories(directory);
      real_controller(argv[1], directory);
      bounded_transport(argv[1], directory);
    }
    std::cout << "viewer live tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
