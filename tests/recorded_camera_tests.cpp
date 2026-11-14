#include "pvd/writer.hpp"
#include "recorded_camera.hpp"
#include "scene_tools.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
void near(Vec3 a, Vec3 b, const char *m) {
  check(std::abs(a.x - b.x) < 1e-5f && std::abs(a.y - b.y) < 1e-5f &&
            std::abs(a.z - b.z) < 1e-5f,
        m);
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  template <class T> void emit(T value) {
    events.push_back(
        {1, events.size(), events.size() * 7, Payload{std::move(value)}});
  }
  void mark(bool begin) {
    if (begin)
      emit(BeginSection{1, 3, events.size()});
    else
      emit(EndSection{1, 3, events.size()});
  }
  explicit Fixture(ByteOrder order) {
    header.byte_order = order;
    header.stream_flags = 1;
    emit(StringHandleEvent{"physx3", 1});
    emit(StringHandleEvent{"PxScene", 2});
    emit(StringHandleEvent{"frame", 3});
    emit(CreateClass{{1, 2}});
    emit(CreateInstance{{1, 2}, 1});
    mark(true);
    mark(false);
    mark(true);
    emit(SetCamera{"Clip", {1, 3, 9}, {0, 1, 0}, {1, 0, 0}});
    mark(false);
    mark(true);
    emit(OriginShift{1, {100, 2, -3}});
    mark(false);
    mark(true);
    emit(SetCamera{"Clip", {0, 0, 0}, {0, 1, 0}, {0, 0, 0}});
    mark(false);
    mark(true);
    emit(SetCamera{"Clip", {4, 6, 12}, {0, 0, 1}, {7, 4, 2}});
    mark(false);
    mark(true);
    emit(SetCamera{"Other", {-4, 2, 7}, {1, 1, 0}, {0, 0, 0}});
    mark(false);
  }
  void load(Document &doc) const {
    SessionOptions options;
    options.checkpoint_interval = 3;
    doc.adopt(std::make_unique<Session>(header, events, options),
              "recorded camera fixture");
    doc.set_scene(1);
    doc.set_frame(0);
  }
};
void run(ByteOrder order) {
  Fixture f(order);
  Document doc;
  f.load(doc);
  OrbitCamera camera;
  camera.target = {9, 8, 7};
  SceneToolState state;
  const auto initial = camera.position();
  std::vector<Bytes> wire;
  for (const auto &e : doc.session()->events())
    wire.push_back(writing::encode_event(e, order));
  check(followRecordedCamera(doc, "Clip", camera, state),
        "missing camera name did not bind");
  check(state.recorded_follow.enabled &&
            state.recorded_follow.status == RecordedCameraStatus::missing,
        "missing status");
  near(camera.position(), initial, "missing camera changed view");
  camera.near_plane = .03;
  camera.far_plane = 999;
  camera.vertical_fov = .8;
  doc.set_frame(1);
  check(updateTrackedCamera(doc, camera, state),
        "forward follow did not update");
  near(camera.position(), {1, 3, 9}, "forward camera eye");
  near(camera.target, {1, 0, 0}, "forward camera target");
  check(camera.near_plane == .03 && camera.far_plane == 999 &&
            camera.vertical_fov == .8,
        "follow overwrote unrecorded projection settings");
  const auto frame = doc.frame();
  doc.set_frame(2);
  updateTrackedCamera(doc, camera, state);
  near(camera.position(), {1, 3, 9},
       "follow guessed OriginShift for scene-less named camera");
  near(doc.frame()->snapshot.cameras.at("Clip").position, {1, 3, 9},
       "raw camera mutated");
  doc.set_frame(3);
  check(!updateTrackedCamera(doc, camera, state), "invalid camera applied");
  check(state.recorded_follow.enabled &&
            state.recorded_follow.status == RecordedCameraStatus::invalid,
        "invalid camera status");
  near(camera.position(), {1, 3, 9}, "invalid camera partially changed view");
  doc.set_frame(4);
  check(updateTrackedCamera(doc, camera, state),
        "same named camera did not recover");
  near(camera.position(), {4, 6, 12}, "recreated name eye");
  near(camera.reference_up, {0, 0, 1}, "recorded up lost");
  doc.set_frame(1);
  check(updateTrackedCamera(doc, camera, state), "reverse seek follow failed");
  near(camera.position(), {1, 3, 9}, "reverse camera eye");
  doc.set_frame(0);
  check(!updateTrackedCamera(doc, camera, state) &&
            state.recorded_follow.enabled,
        "reverse missing camera did not keep binding");
  near(camera.position(), {1, 3, 9}, "missing prefix did not keep last view");
  doc.set_playback({true, false, 10});
  check(doc.advance(.4), "playback did not advance");
  check(updateTrackedCamera(doc, camera, state),
        "playback follow did not resume");
  doc.set_playing(false);
  near(camera.position(), {4, 6, 12},
       "playback camera differs from recorded frame");
  camera.orbit(.2, 0);
  check(!updateTrackedCamera(doc, camera, state) &&
            !state.recorded_follow.enabled,
        "unannounced manual pose change not released");
  check(followRecordedCamera(doc, "Clip", camera, state), "restart follow");
  notifyManualCameraInput(state);
  camera.pan(2, 3, 600);
  check(!state.recorded_follow.enabled &&
            state.recorded_follow.status == RecordedCameraStatus::released,
        "manual hook not released");
  check(followRecordedCamera(doc, "Clip", camera, state),
        "bind before replacement");
  for (std::size_t i = 0; i < wire.size(); ++i)
    check(wire[i] == writing::encode_event(doc.session()->events()[i], order),
          "follow mutated the live source Session events");
  const auto before_replacement = camera.position();
  f.load(doc);
  check(!updateTrackedCamera(doc, camera, state) &&
            !state.recorded_follow.enabled &&
            state.recorded_follow.status ==
                RecordedCameraStatus::source_changed,
        "source replacement reused follow");
  near(camera.position(), before_replacement,
       "source replacement applied unrelated camera");
  near(frame->snapshot.cameras.at("Clip").position, {1, 3, 9},
       "published frame source camera changed");
  for (std::size_t i = 0; i < wire.size(); ++i)
    check(wire[i] == writing::encode_event(f.events[i], order),
          "follow mutated original source events");
  doc.set_frame(1);
  followRecordedCamera(doc, "Clip", camera, state);
  check(applyRecordedCamera(doc, "Clip", camera, state) && !state.tracking(),
        "Apply once did not release follow");
  doc.set_frame(4);
  check(!updateTrackedCamera(doc, camera, state),
        "Apply once became persistent");
  near(camera.position(), {1, 3, 9}, "one-shot apply changed with playback");
  followRecordedCamera(doc, "Clip", camera, state);
  stopTracking(state);
  check(!state.tracking(), "Stop tracking left Clip follow enabled");
}
void real(const char *path) {
  Document doc;
  doc.open_file(path);
  check(doc.timeline().size() >= 4,
        "origin SDK fixture has too few native frames");
  doc.set_frame(0);
  OrbitCamera camera;
  SceneToolState state;
  check(followRecordedCamera(doc, "recorded_scene0", camera, state),
        "actual SDK named camera missing");
  for (const auto index : {std::size_t(0), std::size_t(1), std::size_t(3),
                           std::size_t(2), std::size_t(0)}) {
    doc.set_frame(index);
    updateTrackedCamera(doc, camera, state);
    check(state.recorded_follow.status == RecordedCameraStatus::following,
          "SDK camera follow status");
    near(camera.position(), index < 2 ? Vec3{100, 10, -20} : Vec3{0, 8, -17},
         "SDK camera did not follow exact explicit updates / guessed origin "
         "shift");
    near(camera.target, index < 2 ? Vec3{100, 10, 0} : Vec3{0, 8, 3},
         "SDK camera target changed");
  }
  std::cout << "actual SDK named camera updates/origin-shift independence/seek "
               "passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    run(ByteOrder::little);
    run(ByteOrder::big);
    if (argc > 1)
      real(argv[1]);
    std::cout << "recorded camera model: "
                 "seek/playback/missing/invalid/recreated/source/manual/raw "
                 "fidelity passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
