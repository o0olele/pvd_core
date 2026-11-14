#include "clip_range_fixture.hpp"
#include "pvd/writer.hpp"
#include "workspace.hpp"
#include "workspace_v1_fixture.hpp"
#include "workspace_v2_fixture.hpp"
#include "workspace_v3_fixture.hpp"
#include "workspace_v4_fixture.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {
using namespace pvd;
using namespace pvd::viewer;
void check(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
template <class F> void rejects(F fn, const char *message) {
  try {
    fn();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}
struct Fixture {
  static constexpr std::uint64_t scene = 0xfedcba9876543210ull;
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  void emit(Payload p) {
    events.push_back({1, events.size() * 10, 0, std::move(p)});
  }
  std::uint32_t str(const std::string &s) {
    if (s.empty())
      return 0;
    auto i = handles.find(s);
    if (i != handles.end())
      return i->second;
    auto h = static_cast<std::uint32_t>(handles.size() + 1);
    handles[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *n) {
    auto ns = str("physx3"), h = str(n);
    return {ns, h};
  }
  void prop(const char *c, const char *n, const char *dt, bool array = false) {
    auto cls = type(c), t = type(dt);
    auto h = str(n);
    emit(CreateProperty{
        cls, h, 0, t, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void instance(const char *c, std::uint64_t id) {
    emit(CreateInstance{type(c), id});
  }
  void value(std::uint64_t id, const char *p, const char *t, Bytes b,
             unsigned count = 1) {
    auto h = str(p);
    auto dt = type(t);
    emit(SetPropertyValue{id, h, dt, count, std::move(b)});
  }
  static Bytes floats(std::initializer_list<float> values) {
    Bytes b;
    for (float v : values) {
      std::uint32_t n;
      std::memcpy(&n, &v, 4);
      for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
    return b;
  }
  void ref(std::uint64_t id, const char *p, std::uint64_t target) {
    Bytes b;
    for (unsigned i = 0; i < 8; ++i)
      b.push_back(static_cast<std::uint8_t>(target >> (8 * i)));
    value(id, p, "VoidPtr", std::move(b));
  }
  void pose(std::uint64_t id, const char *p, float x) {
    value(id, p, "PxTransform", floats({0, 0, 0, 1, x, 0, 0}));
  }
  explicit Fixture(bool changed = false) {
    header.stream_flags = 1;
    header.timestamp_numerator = 1;
    for (auto c : {"PxScene", "PxRigidStatic", "PxShape", "PxBoxGeometry"})
      emit(CreateClass{type(c)});
    prop("PxRigidStatic", "Scene", "ObjectRef");
    prop("PxRigidStatic", "Shapes", "ObjectRef", true);
    prop("PxRigidStatic", "GlobalPose", "PxTransform");
    prop("PxShape", "Geometry", "ObjectRef");
    prop("PxShape", "LocalPose", "PxTransform");
    prop("PxShape", "Flags", "PvdU8");
    prop("PxBoxGeometry", "HalfExtents", "PxVec3");
    instance("PxScene", scene);
    instance("PxBoxGeometry", 200);
    value(200, "HalfExtents", "PxVec3", floats({changed ? 2.f : 1.f, 1, 1}));
    instance("PxShape", 100);
    ref(100, "Geometry", 200);
    pose(100, "LocalPose", 0);
    value(100, "Flags", "PvdU8", Bytes{11});
    instance("PxRigidStatic", 10);
    ref(10, "Scene", scene);
    ref(10, "Shapes", 100);
    pose(10, "GlobalPose", 0);
    auto native = str("frame"), after = str("validation_after_fetch");
    for (unsigned i = 0; i < 3; ++i) {
      emit(BeginSection{scene, native, events.size()});
      pose(10, "GlobalPose", float(i));
      emit(EndSection{scene, native, events.size()});
      emit(BeginSection{scene, after, events.size()});
      pose(10, "GlobalPose", float(i) + .5f);
      emit(EndSection{scene, after, events.size()});
    }
  }
  std::unique_ptr<Document> document() const {
    auto d = std::make_unique<Document>();
    d->adopt(std::make_unique<Session>(header, events), "fixture");
    return d;
  }
};
void camera_equal(const OrbitCamera &a, const OrbitCamera &b) {
  check(a.target.x == b.target.x && a.target.y == b.target.y &&
            a.target.z == b.target.z && a.reference_up.x == b.reference_up.x &&
            a.reference_up.y == b.reference_up.y &&
            a.reference_up.z == b.reference_up.z && a.yaw == b.yaw &&
            a.pitch == b.pitch && a.distance == b.distance &&
            a.vertical_fov == b.vertical_fov && a.near_plane == b.near_plane &&
            a.far_plane == b.far_plane && a.handedness == b.handedness,
        "camera persistence lost values/reference up");
}
bool preferences_equal(const ViewPreferences &a, const ViewPreferences &b) {
  const auto rendering = [](const RenderOptions &r) {
    return std::make_tuple(
        r.background_color, r.antialiasing, r.point_size,
        r.contact_normal_length, r.particle_velocity_scale,
        r.rigid_velocity_scale, r.show_bounds, r.show_linear_velocity,
        r.show_angular_velocity, r.show_grid, r.show_axes, r.show_solids,
        r.show_wireframe, r.show_debug_points, r.show_debug_lines,
        r.show_debug_triangles, r.show_contacts, r.show_joint_frames,
        r.show_joint_limits, r.show_scene_queries, r.show_query_normals,
        r.show_query_volumes, r.show_cloth, r.show_particles,
        r.show_cloth_constraints, r.show_cloth_pins, r.show_particle_velocity,
        r.bounds_scope, r.velocity_scope, r.scene_query_scope,
        r.coordinate_frame_scope, r.transparent_scope, r.show_actor_frames,
        r.show_shape_frames, r.show_mass_frames, r.coordinate_frame_scale,
        r.transparent_opacity, r.face_culling, r.show_orbit_target);
  };
  return rendering(a.rendering) == rendering(b.rendering) &&
         std::tie(a.inspection.number_format, a.inspection.float_precision,
                  a.inspection.double_precision, a.inspection.array_page_size,
                  a.inspection.integer_hex,
                  a.show_debug_text, a.navigation.mode,
                  a.navigation.movement_speed, a.navigation.look_sensitivity) ==
             std::tie(b.inspection.number_format, b.inspection.float_precision,
                      b.inspection.double_precision,
                      b.inspection.array_page_size, b.inspection.integer_hex,
                      b.show_debug_text,
                      b.navigation.mode, b.navigation.movement_speed,
                      b.navigation.look_sensitivity);
}
bool view_options_equal(const ViewOptions &a,const ViewOptions &b) {
  if(std::tie(a.show_static,a.show_dynamic,a.show_kinematic,a.show_sleeping,a.show_trigger,
      a.color_mode,a.uniform_color.r,a.uniform_color.g,a.uniform_color.b,a.uniform_color.a,
      a.sleeping_scope,a.trigger_scope,a.geometry_scale,a.show_planes,a.joint_frame_scale,
      a.joint_limit_scale,a.palette.profile_by_event)!=
     std::tie(b.show_static,b.show_dynamic,b.show_kinematic,b.show_sleeping,b.show_trigger,
      b.color_mode,b.uniform_color.r,b.uniform_color.g,b.uniform_color.b,b.uniform_color.a,
      b.sleeping_scope,b.trigger_scope,b.geometry_scale,b.show_planes,b.joint_frame_scale,
      b.joint_limit_scale,b.palette.profile_by_event))return false;
  for(auto role:color_palette_roles)
    if(palette_color(a.palette,role)!=palette_color(b.palette,role))return false;
  return true;
}
ViewPreferences distinct_preferences(bool second = false) {
  ViewPreferences v;
  auto &r = v.rendering;
  r.background_color =
      second ? std::array<float, 4>{1, .75f, .5f, 0}
             : std::array<float, 4>{0, std::nextafter(.25f, 1.f), .5f, 1};
  r.antialiasing = second ? Antialiasing::off : Antialiasing::msaa4x;
  r.show_grid = r.show_axes = r.show_solids = second;
  r.show_wireframe = !second;
  r.show_debug_points = r.show_debug_lines = r.show_debug_triangles = second;
  r.show_contacts = r.show_joint_frames = r.show_joint_limits = second;
  r.show_scene_queries = r.show_query_normals = r.show_query_volumes = second;
  r.show_bounds = r.show_linear_velocity = r.show_angular_velocity = !second;
  r.show_cloth = r.show_particles = r.show_cloth_constraints =
      r.show_cloth_pins = second;
  r.show_particle_velocity = !second;
  r.point_size = second ? 64.f : std::nextafter(1.f, 2.f);
  r.contact_normal_length = second ? 1000000.f : std::nextafter(.125f, 1.f);
  r.particle_velocity_scale = second ? 1000000.f : std::nextafter(.5f, 1.f);
  r.rigid_velocity_scale = second ? .875f : 0;
  r.bounds_scope =
      second ? VisualizationScope::none : VisualizationScope::selected;
  r.velocity_scope =
      second ? VisualizationScope::unselected : VisualizationScope::all;
  r.scene_query_scope =
      second ? VisualizationScope::selected : VisualizationScope::unselected;
  r.coordinate_frame_scope =
      second ? VisualizationScope::all : VisualizationScope::selected;
  r.transparent_scope =
      second ? VisualizationScope::selected : VisualizationScope::unselected;
  r.show_actor_frames = r.show_mass_frames = !second;
  r.show_shape_frames = second;
  r.coordinate_frame_scale = second ? 1000000.f : std::nextafter(.5f, 1.f);
  r.transparent_opacity = second ? 1.f : std::nextafter(.25f, 1.f);
  r.face_culling = second ? FaceCulling::front : FaceCulling::back;
  r.show_orbit_target = !second;
  v.inspection.number_format =
      second ? NumberFormat::fixed : NumberFormat::scientific;
  v.inspection.float_precision = second ? 17 : 3;
  v.inspection.double_precision = second ? 1 : 12;
  v.inspection.array_page_size = second ? 128 : 2048;
  v.inspection.integer_hex = !second;
  v.show_debug_text = second;
  v.navigation.mode =
      second ? CameraNavigationMode::orbit : CameraNavigationMode::first_person;
  v.navigation.movement_speed = second ? 0 : std::nextafter(123.0, 124.0);
  v.navigation.look_sensitivity = second ? .1 : std::nextafter(.003, .004);
  return v;
}
void ownership_and_locks() {
  Fixture f;
  Workspace workspace;
  auto a = workspace.add(f.document(), "a.pxd2"),
       b = workspace.add(f.document(), "b.pxd2");
  auto *stable = workspace.find(a);
  auto *document = &stable->document();
  check(document->frame()->draws.size() == 1,
        "real model fixture shape missing");
  document->set_frame(1);
  auto key = document->frame()->draws[0].key;
  check(document->select(key), "fixture selection failed");
  stable->camera().distance = 123;
  workspace.find(b)->camera().distance = 456;
  for (unsigned i = 0; i < 10; ++i)
    workspace.create_empty();
  check(workspace.find(a) == stable && &stable->document() == document,
        "adding slots moved existing document");
  workspace.activate(b);
  workspace.find(b)->document().set_frame(2);
  workspace.activate(a);
  check(document->frame_index() == 1 && document->selected_key() == key &&
            stable->camera().distance == 123 &&
            workspace.find(b)->camera().distance == 456,
        "slot switch mixed frame/selection/camera state");
  for (auto reason : {SlotLock::loading, SlotLock::live, SlotLock::exporting}) {
    check(workspace.lock(a, reason) && workspace.any_locked(), "lock failed");
    auto replacement = f.document();
    auto *pending = replacement.get();
    check(!workspace.close(a) &&
              !workspace.replace(a, replacement, "new.pxd2") &&
              replacement.get() == pending && &stable->document() == document,
          "locked document was closed/replaced or incoming consumed");
    check(workspace.activate(b) && workspace.active_id() == b,
          "locked source prevented unrelated activation");
    check(!workspace.lock(a, reason), "second exclusive lock accepted");
    auto wrong =
        reason == SlotLock::live ? SlotLock::exporting : SlotLock::live;
    check(!workspace.unlock(a, wrong), "wrong owner unlocked slot");
    auto state = workspace.settings();
    rejects([&] { workspace.restore(state); },
            "locked workspace replacement accepted");
    check(workspace.unlock(a, reason), "matching unlock failed");
  }
  auto replacement = f.document();
  check(workspace.replace(a, replacement, "a.pxd2") && !replacement &&
            workspace.find(a) == stable && &stable->document() != document &&
            stable->document().frame_index() == 1,
        "unlocked replacement not stable or view lost");
  WorkspaceLimits limits;
  limits.max_slots = 1;
  Workspace bounded({}, limits);
  bounded.create_empty();
  rejects([&] { bounded.create_empty(); }, "slot limit ignored");
  WorkspaceLimits recent_limits;
  recent_limits.max_recent_files = 2;
  Workspace recent({}, recent_limits);
  for (auto p : {"C", "A", "B", "A"})
    recent.remember_recent(p);
  check(recent.recent_files() == std::vector<std::string>{"A", "B"},
        "recent order/dedup/limit failed");
}
void bookmarks_and_identity() {
  Fixture original, changed(true);
  Workspace w;
  auto a = w.add(original.document(), "same.pxd2");
  auto *first = w.find(a);
  first->document().set_sample_point(SamplePoint::after_fetch);
  first->document().set_frame(2);
  first->camera().set_look_at({{5, 2, 10}, {0, 0, 1}, {0, 0, 0}});
  auto cam = first->camera();
  auto book = w.add_bookmark(a, u8"Impact \u4e66\u7b7e", true);
  auto old_identity = *first->source_identity();
  auto other = w.add(original.document(), "same.pxd2");
  check(w.find(other)->bookmarks().size() == 1 &&
            w.find(other)->bookmark_verified(book),
        "bookmarks not shared by matching capture");
  auto changed_document = changed.document();
  auto new_identity = workspace_source_identity(*changed_document->session());
  check(new_identity.event_count == old_identity.event_count &&
            new_identity.fingerprint != old_identity.fingerprint,
        "canonical hash missed same-length changed payload");
  std::vector<std::string> warnings;
  check(w.replace(a, changed_document, "same.pxd2", &warnings, new_identity),
        "changed capture replacement failed");
  check(!warnings.empty() && !w.find(a)->warning().empty() &&
            !w.find(a)->bookmark_verified(book) &&
            w.find(a)->bookmarks().size() == 1,
        "changed source did not retain old bookmark as unverified");
  check(w.find(a)->document().frame_index() == 0 &&
            w.find(a)->document().sample_point() == SamplePoint::native_frame &&
            !w.find(a)->camera_ready(),
        "old view was silently applied to changed capture");
  auto before = w.find(a)->document().frame();
  check(!w.jump_to_bookmark(a, book) && w.find(a)->document().frame() == before,
        "unverified bookmark mutated view");
  check(w.find(other)->bookmark_verified(book) &&
            w.jump_to_bookmark(other, book),
        "still-open original Session lost matching bookmark");
  camera_equal(w.find(other)->camera(), cam);
  auto fresh = w.add_bookmark(a, "New file version", false);
  check(w.find(a)->bookmark_verified(fresh) &&
            !w.find(other)->bookmark_verified(fresh),
        "per-bookmark identities were mixed");
  auto saved = decode_workspace(encode_workspace(w.settings()));
  Workspace restored;
  auto pending = restored.restore(saved);
  check(pending.source_slots.size() == 2 &&
            !restored.find(a)->document().loaded(),
        "restore should create source placeholders only");
  for (auto id : pending.source_slots) {
    auto d = changed.document();
    std::vector<std::string> issues;
    check(restored.replace(id, d, "same.pxd2", &issues),
          "placeholder load failed");
    if (id == other)
      check(!issues.empty() && restored.find(id)->document().frame_index() == 0,
            "original-file saved view was applied to replacement file");
    check(!restored.find(id)->bookmark_verified(book) &&
              restored.find(id)->bookmark_verified(fresh),
          "persisted bookmark identity verification failed");
  }
  restored.find(a)->document().set_frame(1);
  check(restored.close(a), "close failed");
  auto reopened = restored.add(changed.document(), "same.pxd2");
  check(restored.find(reopened)->document().frame_index() == 1 &&
            restored.find(reopened)->bookmarks().size() == 2,
        "per-capture state lost after close/reopen");
  // Repacking/byte offsets do not change source identity.
  auto d = original.document();
  std::ostringstream out;
  writing::write_session(out, *d->session());
  auto text = out.str();
  auto reparsed = Session::parse(Bytes(text.begin(), text.end()));
  check(workspace_source_identity(reparsed) ==
            workspace_source_identity(*d->session()),
        "wire repacking changed canonical source fingerprint");
}
void rejected_identity_does_not_reserve_archive() {
  Fixture fixture;
  WorkspaceLimits limits;
  limits.max_captures = 1;
  Workspace workspace({}, limits);
  const auto target = workspace.create_empty();
  auto incoming = fixture.document();
  auto *candidate = incoming.get();
  auto wrong = workspace_source_identity(*incoming->session());
  ++wrong.event_count;
  rejects(
      [&] { workspace.replace(target, incoming, "bad.pxd2", nullptr, wrong); },
      "invalid precomputed identity accepted");
  check(incoming.get() == candidate && workspace.settings().captures.empty(),
        "rejected identity consumed candidate or leaked archive quota");
  check(workspace.replace(target, incoming, "retry.pxd2") && !incoming &&
            workspace.settings().captures.size() == 1,
        "valid retry failed after rejected identity");
  Workspace added({}, limits);
  rejects([&] { added.add(fixture.document(), "bad.pxd2", {}, wrong); },
          "add accepted invalid identity");
  check(added.settings().captures.empty() && added.slots().empty(),
        "failed add leaked an archive or slot");
  check(added.add(fixture.document(), "retry.pxd2") != 0,
        "valid add failed after rejected identity");
}
void bookmark_camera_collision_preserves_both_views() {
  Fixture fixture;
  Workspace seed;
  auto slot = seed.add(fixture.document(), "source-A.pxd2");
  seed.find(slot)->camera().target = {1, 2, 3};
  seed.add_bookmark(slot, "Same recorded frame", true);
  auto settings = seed.settings();
  auto second = settings.captures.front();
  second.source_path = "source-B.pxd2";
  second.bookmarks[0].camera->target = {9, 8, 7};
  second.bookmarks[0].camera->handedness = DisplayHandedness::left_handed;
  settings.captures.push_back(second);
  Workspace restored;
  restored.restore(settings);
  auto incoming = fixture.document();
  check(restored.replace(slot, incoming, "source-A.pxd2"),
        "camera-collision source load");
  check(restored.bind_source(slot, "source-B.pxd2"),
        "camera-collision source bind");
  const auto &books = restored.find(slot)->bookmarks();
  check(books.size() == 2 && books[0].id != books[1].id && books[0].camera &&
            books[1].camera,
        "camera-distinct colliding bookmarks were merged");
  check(books[0].camera->target.x == 9 && books[1].camera->target.x == 1 &&
            books[0].camera->handedness == DisplayHandedness::left_handed &&
            books[1].camera->handedness == DisplayHandedness::right_handed,
        "bookmark camera fields were not preserved exactly");
  check(restored.bind_source(slot, "source-B.pxd2") &&
            restored.find(slot)->bookmarks().size() == 2,
        "rebinding the same archive duplicated bookmarks");
}
void stopped_live_binding() {
  Fixture f;
  Workspace w;
  auto target = w.create_empty("Live capture");
  auto other = w.create_empty();
  auto *slot = w.find(target);
  auto *doc = &slot->document();
  doc->begin_live(f.header, "TCP 127.0.0.1");
  check(w.lock(target, SlotLock::live), "live slot lock");
  doc->append_live(f.events);
  auto *session = doc->session();
  auto book = w.add_bookmark(target, "Live moment", true);
  w.activate(other);
  check(!w.bind_source(target, "recorded.pxd2"),
        "live source was bound while locked/receiving");
  doc->finish_live();
  check(!w.bind_source(target, "recorded.pxd2"),
        "locked stopped source was bound");
  w.unlock(target, SlotLock::live);
  auto id = workspace_source_identity(*session);
  check(w.bind_source(target, "recorded.pxd2", id),
        "completed raw source could not bind");
  check(&slot->document() == doc && doc->session() == session &&
            w.active_id() == other,
        "binding moved Session/Document or redirected active receiver target");
  check(slot->bookmark_verified(book) &&
            slot->source_path() == "recorded.pxd2" &&
            w.recent_files().front() == "recorded.pxd2",
        "binding did not persist live bookmarks/path");
  auto state = w.settings();
  check(state.slots.size() == 1 && state.slots[0].id == target &&
            state.captures[0].bookmarks[0].identity == id,
        "transient slots persisted or bound identity lost");
  auto wrong = id;
  ++wrong.event_count;
  rejects([&] { w.bind_source(target, "wrong.pxd2", wrong); },
          "stale precomputed identity accepted");
}
void preferences_and_source_identity() {
  Fixture original, changed(true);
  const auto defaults = distinct_preferences(),
             saved = distinct_preferences(true);
  const ViewPreferences builtin;
  Workspace w;
  w.set_default_preferences(defaults);
  const auto empty = w.create_empty();
  const auto a = w.add(original.document(), "prefs.pxd2");
  check(preferences_equal(w.find(empty)->preferences(), defaults) &&
            preferences_equal(w.find(a)->preferences(), defaults),
        "new empty/source slots did not inherit global preferences");
  w.find(a)->preferences() = saved;
  w.set_default_preferences(builtin);
  const auto another = w.add(original.document(), "new.pxd2");
  check(preferences_equal(w.find(a)->preferences(), saved) &&
            preferences_equal(w.find(empty)->preferences(), defaults) &&
            preferences_equal(w.find(another)->preferences(), builtin),
        "global defaults mutated existing slots or were not inherited");
  auto incoming = original.document();
  check(w.replace(a, incoming, "prefs.pxd2") &&
            preferences_equal(w.find(a)->preferences(), saved),
        "same-identity replacement lost slot preferences");
  const auto book = w.add_bookmark(a, "Camera only", true);
  const auto camera = w.find(a)->camera();
  w.find(a)->preferences() = defaults;
  w.find(a)->camera().distance = 99;
  check(w.jump_to_bookmark(a, book) &&
            preferences_equal(w.find(a)->preferences(), defaults),
        "camera bookmark unexpectedly restored view preferences");
  camera_equal(w.find(a)->camera(), camera);
  w.find(a)->preferences() = saved;
  check(w.close(a), "close preferences source");
  const auto reopened = w.add(original.document(), "prefs.pxd2");
  check(preferences_equal(w.find(reopened)->preferences(), saved),
        "matching archived last_view preferences were not restored");
  auto wrong = *w.find(reopened)->source_identity();
  ++wrong.event_count;
  auto invalid_document = original.document();
  auto *unchanged = invalid_document.get();
  rejects(
      [&] {
        w.replace(reopened, invalid_document, "never-reserved.pxd2", nullptr,
                  wrong);
      },
      "invalid precomputed identity accepted while restoring preferences");
  check(
      invalid_document.get() == unchanged &&
          preferences_equal(w.find(reopened)->preferences(), saved),
      "invalid identity changed slot preferences or consumed incoming source");
  incoming = changed.document();
  std::vector<std::string> warnings;
  check(w.replace(reopened, incoming, "prefs.pxd2", &warnings) &&
            !warnings.empty() &&
            preferences_equal(w.find(reopened)->preferences(), builtin),
        "changed identity reused old view preferences instead of defaults");

  Workspace seed;
  seed.set_default_preferences(defaults);
  const auto stored = seed.add(original.document(), "stored.pxd2");
  seed.find(stored)->preferences() = saved;
  const auto encoded = encode_workspace(seed.settings());
  const auto decoded = decode_workspace(encoded);
  check(preferences_equal(decoded.default_preferences, defaults) &&
            preferences_equal(decoded.slots[0].view.preferences, saved) &&
            decoded.captures[0].last_view &&
            preferences_equal(decoded.captures[0].last_view->preferences,
                              saved) &&
            encode_workspace(decoded) == encoded,
        "v2 did not exactly round-trip all slot/archive/default preference "
        "fields");
  Workspace restored;
  restored.restore(decoded);
  check(preferences_equal(restored.default_preferences(), defaults) &&
            preferences_equal(restored.find(stored)->preferences(), saved),
        "placeholder restore lost defaults or pending preference state");
  incoming = original.document();
  check(restored.replace(stored, incoming, "stored.pxd2") &&
            preferences_equal(restored.find(stored)->preferences(), saved),
        "verified restored source did not retain preferences");
  restored.restore(decoded);
  incoming = changed.document();
  check(restored.replace(stored, incoming, "stored.pxd2") &&
            preferences_equal(restored.find(stored)->preferences(), defaults),
        "pending view preferences applied to a changed capture identity");
  const auto fallback = restored.create_empty();
  check(preferences_equal(restored.find(fallback)->preferences(), defaults),
        "restored global defaults were not applied to a new empty slot");

  const auto live = seed.create_empty("Recorded live");
  auto *slot = seed.find(live);
  slot->document().begin_live(original.header);
  slot->document().append_live(original.events);
  slot->document().finish_live();
  slot->preferences() = saved;
  const auto *stable = &slot->document();
  check(seed.bind_source(live, "stored.pxd2") && &slot->document() == stable &&
            preferences_equal(slot->preferences(), saved),
        "binding a recording replaced its current preferences or Document");
  check(seed.close(live), "close bound recording");
  const auto bound_reopen = seed.add(original.document(), "stored.pxd2");
  check(preferences_equal(seed.find(bound_reopen)->preferences(), saved),
        "bound recording preference archive was not recoverable");
}
WorkspaceSettings minimal() {
  Fixture f;
  Workspace w;
  auto id = w.add(f.document(), u8"D:/\u6355\u83b7/\U0001F680.pxd2",
                  u8"\u8c03\u8bd5 capture");
  auto *s = w.find(id);
  s->document().set_frame(2);
  w.set_default_preferences(distinct_preferences());
  s->preferences() = distinct_preferences(true);
  s->camera().set_look_at({{5, 2, 10}, {0, 0, 1}, {0, 0, 0}});
  s->camera().distance = std::nextafter(123.0, 124.0);
  s->camera().handedness = DisplayHandedness::left_handed;
  auto options = s->document().view_options();
  options.show_static = false;
  options.color_mode = ColorMode::uniform;
  options.uniform_color = {1, 2, 3, 4};
  options.sleeping_scope = VisualizationScope::selected;
  options.trigger_scope = VisualizationScope::unselected;
  options.geometry_scale = 2;
  options.show_planes = false;
  options.joint_frame_scale = .25f;
  options.joint_limit_scale = 3;
  options.palette.active = {.1f,.2f,.3f,.4f};
  options.palette.profile_by_event = false;
  s->document().set_view_options(options);
  w.add_bookmark(id, u8"\u6d4b\u8bd5, \"quoted\"", true);
  return w.settings();
}
std::uint32_t crc(const Bytes &b, std::size_t offset) {
  std::uint32_t c = ~0u;
  for (std::size_t i = offset; i < b.size(); ++i) {
    c ^= b[i];
    for (unsigned k = 0; k < 8; ++k)
      c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
  }
  return ~c;
}
void fix_crc(Bytes &b) {
  auto n = crc(b, 28);
  for (unsigned i = 0; i < 4; ++i)
    b[20 + i] = static_cast<std::uint8_t>(n >> (8 * i));
}
void invalid_preferences_are_atomic() {
  Fixture fixture;
  Workspace w;
  w.set_default_preferences(distinct_preferences());
  const auto id = w.add(fixture.document(), "validated.pxd2");
  w.find(id)->preferences() = distinct_preferences(true);
  const auto original = w.settings();
  auto *stable = w.find(id);
  const auto baseline = encode_workspace(original);
  std::vector<ViewPreferences> invalid;
  const auto bad = [&](auto change) {
    auto prefs = distinct_preferences();
    change(prefs);
    invalid.push_back(prefs);
  };
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  const auto inf = std::numeric_limits<float>::infinity();
  bad([](auto &v) {
    v.rendering.antialiasing = static_cast<Antialiasing>(99);
  });
  bad([&](auto &v) { v.rendering.background_color[0] = nan; });
  bad([](auto &v) { v.rendering.background_color[1] = -0.1f; });
  bad([](auto &v) { v.rendering.background_color[3] = 1.01f; });
  bad([&](auto &v) { v.rendering.point_size = nan; });
  bad([](auto &v) { v.rendering.point_size = 0; });
  bad([](auto &v) { v.rendering.point_size = 65; });
  bad([&](auto &v) { v.rendering.contact_normal_length = inf; });
  bad([](auto &v) { v.rendering.contact_normal_length = -1; });
  bad([&](auto &v) { v.rendering.particle_velocity_scale = nan; });
  bad([](auto &v) { v.rendering.particle_velocity_scale = -1; });
  bad([](auto &v) { v.rendering.particle_velocity_scale = 1000001.f; });
  bad([&](auto &v) { v.rendering.rigid_velocity_scale = inf; });
  bad([](auto &v) { v.rendering.rigid_velocity_scale = -1; });
  bad([](auto &v) { v.rendering.rigid_velocity_scale = 1000001.f; });
  bad([](auto &v) {
    v.inspection.number_format = static_cast<NumberFormat>(99);
  });
  bad([](auto &v) { v.inspection.float_precision = 0; });
  bad([](auto &v) { v.inspection.double_precision = 18; });
  bad([](auto &v) { v.inspection.array_page_size = 33; });
  bad([](auto &v) { v.rendering.face_culling = static_cast<FaceCulling>(99); });
  bad([](auto &v) {
    v.inspection.array_page_size = std::numeric_limits<std::size_t>::max();
  });
  bad([](auto &v) {
    v.navigation.mode = static_cast<CameraNavigationMode>(99);
  });
  bad([](auto &v) { v.navigation.movement_speed = -1; });
  bad([&](auto &v) {
    v.navigation.look_sensitivity = std::numeric_limits<double>::quiet_NaN();
  });
  bad([](auto &v) {
    v.rendering.bounds_scope = static_cast<VisualizationScope>(99);
  });
  bad([](auto &v) {
    v.rendering.velocity_scope = static_cast<VisualizationScope>(99);
  });
  bad([](auto &v) {
    v.rendering.scene_query_scope = static_cast<VisualizationScope>(99);
  });
  bad([](auto &v) {
    v.rendering.coordinate_frame_scope = static_cast<VisualizationScope>(99);
  });
  bad([](auto &v) {
    v.rendering.transparent_scope = static_cast<VisualizationScope>(99);
  });
  bad([&](auto &v) { v.rendering.coordinate_frame_scale = nan; });
  bad([](auto &v) { v.rendering.coordinate_frame_scale = -1; });
  bad([](auto &v) { v.rendering.coordinate_frame_scale = 1000001; });
  bad([&](auto &v) { v.rendering.transparent_opacity = nan; });
  bad([](auto &v) { v.rendering.transparent_opacity = -.01f; });
  bad([](auto &v) { v.rendering.transparent_opacity = 1.01f; });
  for (const auto &prefs : invalid) {
    check(!valid_view_preferences(prefs), "invalid preferences reported valid");
    rejects([&] { w.set_default_preferences(prefs); },
            "invalid defaults accepted");
    auto settings = original;
    settings.default_preferences = prefs;
    rejects([&] { encode_workspace(settings); }, "invalid defaults encoded");
    rejects([&] { w.restore(settings); },
            "invalid defaults replaced workspace");
    settings = original;
    settings.slots[0].view.preferences = prefs;
    rejects([&] { encode_workspace(settings); },
            "invalid per-slot preferences encoded");
    rejects([&] { w.restore(settings); },
            "invalid per-slot preferences replaced workspace");
    settings = original;
    settings.captures[0].last_view->preferences = prefs;
    rejects([&] { encode_workspace(settings); },
            "invalid archived preferences encoded");
    rejects([&] { w.restore(settings); },
            "invalid archive preferences replaced workspace");
    check(w.find(id) == stable && encode_workspace(w.settings()) == baseline,
          "rejected preferences changed slot/default/archive state");
  }
  WorkspaceLimits limits;
  limits.max_file_bytes = baseline.size() - 1;
  rejects([&] { encode_workspace(original, limits); },
          "preferences bypassed encode byte budget");

  // Hostile but correctly checksummed v4 defaults. The v2/v3 prefix is intact;
  // v5 preferences retain the v4 96-byte prefix, then culling and marker.
  // A complete 186-byte default ViewOptions value follows the preferences.
  const auto empty = encode_workspace(WorkspaceSettings{});
  const auto base = empty.size() - 284;
  auto wire = empty;
  wire[base + 16] = 99;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); }, "wire AA enum bypassed validation");
  wire = empty;
  wire[base + 61] = 2;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); },
          "wire debug-text boolean accepted 2");
  wire = empty;
  wire[base + 50] = 0;
  wire[base + 51] = 0;
  wire[base + 52] = 0xC0;
  wire[base + 53] = 0x7F;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); }, "wire NaN point size accepted");
  wire = empty;
  for (unsigned i = 0; i < 4; ++i)
    wire[base + 57 + i] = 255;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); },
          "wire inspector page count bypassed budget policy");
  wire = empty;
  wire[base + 62] = 99;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); },
          "wire navigation mode bypassed validation");
  wire = empty;
  wire[base + 79] = 99;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); },
          "wire visualization scope bypassed validation");
  wire = empty;
  wire[base + 84] = 2;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); },
          "wire coordinate boolean accepted 2");
  wire = empty;
  wire[base + 95] = 2;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); },
          "wire integer hexadecimal boolean accepted 2");
  for (auto offset : {std::size_t(96),std::size_t(97),std::size_t(114),std::size_t(283)}) {
    wire=empty;wire[base+offset]=99;fix_crc(wire);
    rejects([&] { decode_workspace(wire); }, "invalid v5 cull/marker/plane/palette flag accepted");
  }
  for (auto offset : {std::size_t(110),std::size_t(115),std::size_t(119),std::size_t(123)}) {
    wire=empty;wire[base+offset+2]=0xC0;wire[base+offset+3]=0x7F;fix_crc(wire);
    rejects([&] { decode_workspace(wire); }, "nonfinite v5 scale/palette accepted");
  }
  wire = empty;
  wire[base + 89] = 0xC0;
  wire[base + 90] = 0x7F;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); },
          "wire NaN coordinate scale accepted");

  WorkspaceLimits quota;
  quota.max_captures = 1;
  Workspace bounded({}, quota);
  const auto temporary = bounded.add(fixture.document());
  bounded.find(temporary)->preferences().rendering.point_size = nan;
  rejects([&] { bounded.bind_source(temporary, "invalid-bind.pxd2"); },
          "invalid current preferences archived by bind");
  auto incoming = fixture.document();
  auto *incoming_ptr = incoming.get();
  rejects([&] { bounded.replace(temporary, incoming, "invalid-replace.pxd2"); },
          "invalid current preferences archived by replace");
  check(incoming.get() == incoming_ptr &&
            bounded.find(temporary)->source_path().empty(),
        "rejected preference publication consumed incoming or bound source");
  bounded.find(temporary)->preferences() = distinct_preferences();
  check(bounded.bind_source(temporary, "valid-bind.pxd2"),
        "invalid preference validation leaked archived source quota");
}
void codec_and_corruption() {
  auto settings = minimal();
  const auto camera = settings.slots[0].view.camera;
  settings.slots[0].id = UINT64_MAX;
  settings.active_slot = UINT64_MAX;
  settings.captures[0].bookmarks[0].id = UINT64_MAX;
  settings.captures[0].bookmarks[0].scene_id = 0xfedcba9876543210ull;
  settings.captures[0].bookmarks[0].frame_index = 0x8000000000000001ull;
  auto bytes = encode_workspace(settings);
  auto decoded = decode_workspace(bytes);
  check(
      decoded.slots[0].id == UINT64_MAX && decoded.active_slot == UINT64_MAX &&
          decoded.captures[0].bookmarks[0].id == UINT64_MAX &&
          decoded.captures[0].bookmarks[0].frame_index == 0x8000000000000001ull,
      "64-bit settings values lost precision");
  camera_equal(decoded.slots[0].view.camera, camera);
  check(decoded.slots[0].view.options.color_mode == ColorMode::uniform &&
            !decoded.slots[0].view.options.show_static &&
            decoded.slots[0].view.options.sleeping_scope ==
                VisualizationScope::selected &&
            decoded.slots[0].view.options.trigger_scope ==
                VisualizationScope::unselected &&
            decoded.slots[0].view.options.geometry_scale == 2 &&
            !decoded.slots[0].view.options.show_planes &&
            decoded.slots[0].view.options.joint_frame_scale == .25f &&
            decoded.slots[0].view.options.joint_limit_scale == 3 &&
            decoded.slots[0].view.options.palette.active[3] == .4f &&
            !decoded.slots[0].view.options.palette.profile_by_event &&
            decoded.slots[0].view.options.uniform_color.a == 4,
        "display options lost");
  check(encode_workspace(decoded) == bytes,
        "workspace codec not exact/UTF8 stable");
  for (std::size_t n = 0; n < bytes.size(); ++n) {
    Bytes cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
    rejects([&] { decode_workspace(cut); }, "truncated workspace accepted");
  }
  auto version = bytes;
  version[8] = 99;
  rejects([&] { decode_workspace(version); }, "unknown version accepted");
  auto broken = bytes;
  broken.back() ^= 1;
  rejects([&] { decode_workspace(broken); }, "checksum corruption accepted");
  auto count = bytes;
  for (unsigned i = 0; i < 4; ++i)
    count[36 + i] = 255;
  fix_crc(count);
  rejects([&] { decode_workspace(count); },
          "hostile collection count accepted");
  WorkspaceLimits raised;
  raised.max_slots = UINT32_MAX;
  bool bounded = false;
  try {
    decode_workspace(count, raised);
  } catch (const WorkspaceError &) {
    bounded = true;
  }
  check(bounded,
        "record count attempted allocation beyond available file bytes");
  WorkspaceLimits l;
  l.max_file_bytes = bytes.size() - 1;
  rejects([&] { decode_workspace(bytes, l); }, "bounded read ignored");
  l = {};
  l.max_string_bytes = 2;
  rejects([&] { decode_workspace(bytes, l); }, "string budget ignored");
  auto bad = settings;
  bad.slots[0].title = std::string("\xc0\xaf", 2);
  rejects([&] { encode_workspace(bad); }, "invalid UTF8 accepted");
  bad = settings;
  bad.slots[0].view.camera.handedness = static_cast<DisplayHandedness>(99);
  rejects([&] { encode_workspace(bad); }, "unknown handedness accepted");
  bad = settings;
  bad.slots[0].view.camera.reference_up = {0, 0, 0};
  rejects([&] { encode_workspace(bad); }, "invalid camera accepted");
  bad = settings;
  bad.slots[0].view.options.color_mode = static_cast<ColorMode>(99);
  rejects([&] { encode_workspace(bad); }, "unknown color mode accepted");
  for (bool sleeping : {false, true}) {
    bad = decoded;
    if (sleeping)
      bad.slots[0].view.options.sleeping_scope =
          static_cast<VisualizationScope>(99);
    else
      bad.slots[0].view.options.trigger_scope =
          static_cast<VisualizationScope>(99);
    rejects([&] { encode_workspace(bad); }, "unknown category scope accepted");
  }
  Workspace w;
  w.restore(decoded);
  rejects([&] { w.create_empty(); }, "slot ID wrapped after uint64 maximum");
  Fixture f;
  auto d = f.document();
  check(w.replace(UINT64_MAX, d, decoded.slots[0].source_path),
        "restored max ID load failed");
  auto frame = w.active()->document().frame();
  check(!w.jump_to_bookmark(UINT64_MAX, UINT64_MAX) &&
            w.active()->document().frame() == frame,
        "unavailable bookmark was silently rounded/mutated");
  rejects([&] { w.add_bookmark(UINT64_MAX, "exhausted"); },
          "bookmark ID wrapped after maximum");
}
std::string read(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
void file_and_strong_restore() {
  auto dir = std::filesystem::current_path() / "build" /
             ("workspace-tests-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  auto file = dir / std::filesystem::u8path(u8"\u5de5\u4f5c\u533a.pvdws");
  auto settings = minimal();
  save_workspace_file(file.u8string(), settings);
  auto original = read(file);
  check(encode_workspace(load_workspace_file(file.u8string())) ==
            encode_workspace(settings),
        "atomic file did not reopen");
  auto changed = settings;
  changed.slots[0].title = "different";
  rejects([&] { save_workspace_file(file.u8string(), changed); },
          "workspace silently overwritten");
  check(read(file) == original, "default save changed existing file");
  auto invalid = changed;
  invalid.version = 99;
  rejects([&] { save_workspace_file(file.u8string(), invalid, true); },
          "invalid overwrite accepted");
  check(read(file) == original, "failed overwrite damaged workspace");
  invalid = changed;
  invalid.slots[0].view.preferences.rendering.point_size =
      std::numeric_limits<float>::quiet_NaN();
  rejects([&] { save_workspace_file(file.u8string(), invalid, true); },
          "invalid preferences were published to workspace file");
  check(read(file) == original,
        "invalid preferences damaged existing workspace file");
  save_workspace_file(file.u8string(), changed, true);
  check(load_workspace_file(file.u8string()).slots[0].title == "different",
        "authorized atomic replace failed");
  Workspace w;
  auto old = w.create_empty("unchanged");
  auto *stable = w.find(old);
  auto bad = encode_workspace(settings);
  bad[8] = 77;
  auto corrupt = dir / "bad.pvdws";
  {
    std::ofstream out(corrupt, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bad.data()), bad.size());
  }
  rejects([&] { w.load_file(corrupt.u8string()); },
          "unknown file version replaced workspace");
  check(w.find(old) == stable && w.active_id() == old,
        "failed load did not preserve workspace strongly");
  auto pending = w.load_file(file.u8string());
  check(pending.source_slots.size() == 1 && !w.active()->document().loaded(),
        "workspace load unexpectedly opened raw sources");
  check(w.lock(w.active()->id(), SlotLock::exporting), "restored source lock");
  rejects([&] { w.load_file(file.u8string()); },
          "locked workspace file load accepted");
  for (const auto &entry : std::filesystem::directory_iterator(dir))
    check(entry.path().filename().u8string().find(".pvdws-tmp-") ==
              std::string::npos,
          "private settings temp leaked");
}
clips::State named_clips(const Session &source, clips::SceneKey scene,
                         const char *name) {
  auto catalog = clips::Catalog::create(source, scene);
  catalog.rename(source, catalog.selected()->id, name);
  return catalog.state();
}
void clip_archives_and_pending() {
  Fixture original, altered(true);
  Workspace workspace;
  auto a = workspace.add(original.document(), "clips.pxd2");
  auto *slot_a = workspace.find(a);
  const auto first = named_clips(*slot_a->document().session(),
                                 {Fixture::scene, 0}, "Original clips");
  check(workspace.set_clip_states(a, {first}) &&
            slot_a->clip_states().size() == 1 &&
            slot_a->pending_clip_states().empty() &&
            slot_a->clip_catalogs().size() == 1,
        "loaded source did not accept/cache verified catalogs");
  const auto b = workspace.add(original.document(), "clips.pxd2");
  auto *slot_b = workspace.find(b);
  check(slot_b->clip_states().size() == 1,
        "duplicate tab did not receive per-source catalog");
  const auto revision = slot_b->clip_revision();
  auto edited = clips::Catalog::restore(*slot_a->document().session(),
                                        slot_a->clip_states()[0]);
  edited.split(*slot_a->document().session(), 0);
  check(workspace.set_clip_states(a, {edited.state()}) &&
            slot_b->clip_revision() > revision &&
            slot_b->clip_catalogs().begin()->second.state().clips.size() == 2,
        "command did not synchronize duplicate-tab cached clip values");
  auto bytes = encode_workspace(workspace.settings());
  auto decoded = decode_workspace(bytes);
  check(decoded.version == workspace_format_version && decoded.captures[0].clip_states.size() == 1 &&
            decoded.captures[0].clip_states[0].clips.size() == 2 &&
            encode_workspace(decoded) == bytes,
        "v4 codec did not exactly preserve named catalog values");
  Workspace restored;
  restored.restore(decoded);
  check(restored.find(a)->clip_states().empty() &&
            restored.find(a)->pending_clip_states().size() == 1,
        "placeholder applied a catalog before source verification");
  auto incoming = original.document();
  check(restored.replace(a, incoming, "clips.pxd2") &&
            restored.find(a)->clip_states().size() == 1 &&
            restored.find(a)->pending_clip_states().empty(),
        "matching placeholder source did not promote catalogs");
  incoming = altered.document();
  std::vector<std::string> warnings;
  check(restored.replace(b, incoming, "clips.pxd2", &warnings) &&
            !warnings.empty() && restored.find(b)->clip_states().empty() &&
            restored.find(b)->pending_clip_states().size() == 1 &&
            restored.settings().captures[0].clip_states.size() == 1,
        "changed source applied or discarded unverified old clip metadata");
  const auto second = named_clips(*restored.find(b)->document().session(),
                                  {Fixture::scene, 0}, "Changed-source clips");
  check(restored.set_clip_states(b, {second}) &&
            restored.find(b)->clip_states().size() == 1 &&
            restored.find(b)->pending_clip_states().size() == 1 &&
            restored.find(a)->clip_states()[0].clips.size() == 2 &&
            restored.find(a)->pending_clip_states().size() == 1,
        "ordinary setter overwrote foreign source metadata or another open "
        "source");
  check(restored.settings().captures[0].clip_states.size() == 2,
        "both source versions must remain in the per-path archive");
  const auto file =
      std::filesystem::temp_directory_path() /
      ("workspace-clips-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".pvdws");
  restored.save_file(file.u8string());
  auto saved = load_workspace_file(file.u8string());
  std::filesystem::remove(file);
  check(saved.captures[0].clip_states.size() == 2,
        "file persistence lost pending foreign source catalogs");
  restored.close(a);
  restored.close(b);
  const auto reopened = restored.add(original.document(), "clips.pxd2");
  check(restored.find(reopened)->clip_states().size() == 1 &&
            restored.find(reopened)->pending_clip_states().size() == 1,
        "closed source archive did not restore verified and pending metadata");
  check(restored.set_clip_states(reopened, {}) &&
            restored.find(reopened)->clip_states().empty() &&
            restored.find(reopened)->pending_clip_states().size() == 1 &&
            restored.settings().captures[0].clip_states.size() == 1,
        "clearing active clips silently discarded pending foreign catalogs");
  check(restored.discard_pending_clip_states(reopened) &&
            restored.find(reopened)->pending_clip_states().empty() &&
            restored.settings().captures[0].clip_states.empty(),
        "explicit discard command failed to remove pending metadata");
  std::cout << "Workspace clip codec/pending/duplicate tabs/reopen passed\n";
}
void clip_live_binding_and_merge() {
  Fixture fixture;
  Workspace workspace;
  const auto target = workspace.add(fixture.document(), "recorded-clips.pxd2");
  const auto full = named_clips(*workspace.find(target)->document().session(),
                                {Fixture::scene, 0}, "File range");
  workspace.set_clip_states(target, {full});
  const auto live = workspace.create_empty("Receiving");
  auto *slot = workspace.find(live);
  slot->document().begin_live(fixture.header);
  const auto seed =
      workspace.find(target)->document().session()->frames()[0].event_count;
  slot->document().append_live(std::vector<Event>(
      fixture.events.begin(), fixture.events.begin() + seed));
  check(workspace.lock(live, SlotLock::live), "live source clip lock");
  const auto first = named_clips(*slot->document().session(),
                                 {Fixture::scene, 0}, "Live sealed range");
  check(workspace.set_clip_states(live, {first}),
        "ordinary live lock wrongly forbids sealed clip editing");
  auto future = full;
  future.clips[0].name = "Future source range";
  check(
      workspace.merge_clip_states(live, {future}) &&
          slot->pending_clip_states().size() == 1 &&
          slot->clip_states()[0].clips.size() == 1,
      "not-yet-received seal was applied or lost instead of remaining pending");
  slot->document().append_live(
      std::vector<Event>(fixture.events.begin() + seed, fixture.events.end()));
  check(slot->pending_clip_states().size() == 1,
        "live append silently promoted pending data during a getter/draw");
  for (unsigned edit = 0; edit < 2; ++edit) {
    auto active = slot->clip_states();
    active[0].clips[0].name = "Live sealed range edited " + std::to_string(edit);
    check(workspace.set_clip_states(live, active) &&
              slot->clip_states()[0].clips.size() == 1 &&
              slot->pending_clip_states().size() == 1 &&
              slot->pending_clip_states()[0].clips[0].name ==
                  "Future source range",
          "ordinary active editing after append dropped or silently promoted "
          "a previously pending catalog");
  }
  check(workspace.verify_clip_states(live) &&
            slot->clip_catalogs().size() == 1 &&
            slot->pending_clip_states().empty() &&
            slot->clip_states()[0].clips.size() == 2 &&
            std::any_of(slot->clip_states()[0].clips.begin(),
                         slot->clip_states()[0].clips.end(),
                         [&](const auto &clip) {
                           return clip.binding.source.event_count == seed;
                         }),
        "explicit verification did not promote received data while preserving "
        "original sealed clip");
  slot->document().finish_live();
  auto *document = &slot->document();
  auto *session = document->session();
  check(!workspace.bind_source(live, "recorded-clips.pxd2"),
        "locked stopped source bound clips");
  workspace.unlock(live, SlotLock::live);
  workspace.activate(target);
  const auto old_revision = workspace.find(target)->clip_revision();
  check(workspace.bind_source(live, "recorded-clips.pxd2") &&
            &slot->document() == document &&
            slot->document().session() == session &&
            workspace.active_id() == target &&
            workspace.find(target)->clip_revision() > old_revision,
        "stopped-live clip association moved/retargeted source or missed "
        "duplicate-tab revision");
  const auto &merged = slot->clip_states()[0];
  check(merged.clips.size() == 3 && slot->pending_clip_states().empty(),
        "stop-live merge silently lost one conflicting named range");
  std::set<clips::Id> ids;
  bool sealed = false;
  for (const auto &clip : merged.clips) {
    check(ids.insert(clip.id).second, "merged stable IDs collided");
    sealed = sealed || clip.binding.source.event_count == seed;
  }
  check(sealed, "stop-live binding rewrote original sealed clip identity");
  workspace.close(live);
  workspace.close(target);
  const auto reopened =
      workspace.add(fixture.document(), "recorded-clips.pxd2");
  check(workspace.find(reopened)->clip_states()[0].clips.size() == 3,
        "recorded live clip lists were not archived across close/reopen");
  std::cout << "Workspace live seals/conflict merge/stopped binding passed\n";
}
void clip_pending_shared_source_edits() {
  Fixture fixture;
  Workspace workspace;
  auto complete = fixture.document();
  const auto seed = complete->session()->frames()[0].event_count;
  const auto full = workspace.add(std::move(complete), "shared-prefix.pxd2");
  const auto future = named_clips(*workspace.find(full)->document().session(),
                                  {Fixture::scene, 0}, "Other tab full range");
  workspace.set_clip_states(full, {future});
  auto receiving = std::make_unique<Document>();
  receiving->begin_live(fixture.header);
  receiving->append_live(std::vector<Event>(fixture.events.begin(),
                                            fixture.events.begin() + seed));
  const auto live = workspace.add(std::move(receiving), "shared-prefix.pxd2");
  auto *slot = workspace.find(live);
  check(slot->pending_clip_states().size() == 1,
        "shared-path shorter source should hold the other tab's seal pending");
  const auto first = named_clips(*slot->document().session(),
                                 {Fixture::scene, 0}, "Short prefix range");
  workspace.set_clip_states(live, {first});
  check(workspace.find(full)->clip_states()[0].clips.size() == 2,
        "matching full tab did not retain both original prefix catalogs");
  workspace.lock(live, SlotLock::live);
  slot->document().append_live(std::vector<Event>(fixture.events.begin() + seed,
                                                  fixture.events.end()));
  const auto active_before = workspace.active_id();
  for (unsigned edit = 0; edit < 2; ++edit) {
    auto active = slot->clip_states();
    active[0].clips[0].name = "Short prefix edited " + std::to_string(edit);
    check(workspace.set_clip_states(live, active) &&
              slot->clip_states()[0].clips.size() == 1 &&
              slot->pending_clip_states().size() == 1 &&
              workspace.find(full)->clip_states()[0].clips.size() == 2 &&
              workspace.find(full)->pending_clip_states().empty() &&
              workspace.active_id() == active_before,
          "ordinary edits mixed per-tab pending classification or lost the "
          "same-path archive's other range");
  }
  check(workspace.verify_clip_states(live) &&
            slot->pending_clip_states().empty() &&
            slot->clip_states()[0].clips.size() == 2 &&
            workspace.find(full)->clip_states()[0].clips.size() == 2,
        "explicit verification failed after stable same-path edits");
  std::cout << "Workspace pending append/edits/duplicate source preservation passed\n";
}
void clip_placeholder_limits_and_corruption() {
  Fixture fixture;
  Workspace seed;
  const auto id = seed.add(fixture.document(), "pending-clips.pxd2");
  const auto original = named_clips(*seed.find(id)->document().session(),
                                    {Fixture::scene, 0}, "Clip marker");
  seed.set_clip_states(id, {original});
  const auto settings = seed.settings();
  Workspace placeholder;
  placeholder.restore(settings);
  auto another = original;
  another.clips[0].name = "Imported pending name";
  check(placeholder.merge_clip_states(id, {another}) &&
            placeholder.find(id)->clip_states().empty() &&
            placeholder.find(id)->pending_clip_states().size() == 2,
        "placeholder merge did not retain unverified metadata separately");
  auto incoming = fixture.document();
  check(placeholder.replace(id, incoming, "pending-clips.pxd2") &&
            placeholder.find(id)->clip_states()[0].clips.size() == 2 &&
            placeholder.find(id)->pending_clip_states().empty(),
        "matching load did not preserve/remap both pending clip-ID variants");
  WorkspaceLimits quota;
  quota.max_clip_catalogs_per_capture = 1;
  Workspace bounded({}, quota);
  const auto q = bounded.add(fixture.document(), "bounded-clips.pxd2");
  bounded.set_clip_states(q, {original});
  const auto baseline = encode_workspace(bounded.settings());
  const auto revision = bounded.find(q)->clip_revision();
  rejects([&] { bounded.merge_clip_states(q, {another}); },
          "clip archive catalog budget ignored");
  check(encode_workspace(bounded.settings()) == baseline &&
            bounded.find(q)->clip_revision() == revision,
        "rejected merge changed archive/cache/revision");
  auto invalid = original;
  invalid.clips[0].id = 0;
  rejects([&] { bounded.set_clip_states(q, {invalid}); },
          "invalid active clip ID accepted");
  auto bad = settings;
  bad.captures[0].clip_states[0].clips[0].frames.last = SIZE_MAX;
  rejects([&] { encode_workspace(bad); }, "forged clip bounds encoded");
  bad = settings;
  bad.captures[0].clip_states[0].selected = 123456;
  rejects([&] { placeholder.restore(bad); },
          "invalid selected clip ID restored");
  auto wire = encode_workspace(settings);
  const std::string marker = "Clip marker";
  auto found =
      std::search(wire.begin(), wire.end(), marker.begin(), marker.end());
  check(found != wire.end(), "clip corruption fixture marker");
  const auto name_at = static_cast<std::size_t>(found - wire.begin());
  wire[name_at] = 0xff;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); }, "wire clip invalid UTF8 accepted");
  wire = encode_workspace(settings);
  for (unsigned i = 0; i < 4; ++i)
    wire[name_at - 16 + i] = 255;
  fix_crc(wire);
  rejects([&] { decode_workspace(wire); },
          "hostile clip count bypassed pre-allocation budget");
  const auto all = encode_workspace(settings);
  for (std::size_t n = 0; n < all.size(); ++n) {
    rejects([&] { decode_workspace(Bytes(all.begin(), all.begin() + n)); },
            "truncated v4 clip data accepted");
  }
  std::cout << "Workspace pending/clip budgets/corruption passed\n";
}
void global_display_defaults() {
  Fixture f;
  Workspace workspace;
  ViewOptions first;
  first.geometry_scale=2;first.joint_frame_scale=.5f;first.joint_limit_scale=3;
  first.show_planes=false;first.palette.active={.1f,.2f,.3f,.4f};
  first.palette.profile_by_event=false;
  workspace.set_default_view_options(first);
  const auto saved=workspace.add(f.document(),"display-defaults.pxd2");
  check(view_options_equal(workspace.find(saved)->document().view_options(),first),
        "new source did not inherit complete global display defaults");
  ViewOptions second;
  second.geometry_scale=.25f;second.joint_frame_scale=4;
  second.palette.cloth={.8f,.7f,.6f,.5f};
  workspace.set_default_view_options(second);
  const auto empty=workspace.create_empty();
  check(view_options_equal(workspace.find(empty)->document().view_options(),second) &&
        view_options_equal(workspace.find(saved)->document().view_options(),first),
        "global display changes retroactively modified an existing source");
  workspace.find(empty)->document().begin_live(f.header);
  workspace.find(empty)->document().append_live(f.events);
  check(view_options_equal(workspace.find(empty)->document().view_options(),second) &&
        workspace.find(empty)->document().frame()->draws[0].geometry_scale==.25f,
        "live initialization lost inherited global display defaults");
  workspace.find(empty)->document().finish_live();
  workspace.close(empty);workspace.close(saved);
  const auto reopened=workspace.add(f.document(),"display-defaults.pxd2");
  check(view_options_equal(workspace.find(reopened)->document().view_options(),first),
        "identity-matched archived display did not override global defaults");
  Fixture changed(true);auto incoming=changed.document();
  check(workspace.replace(reopened,incoming,"display-defaults.pxd2") &&
        view_options_equal(workspace.find(reopened)->document().view_options(),second),
        "changed source received a foreign palette/geometry view");
  const auto state=workspace.settings();const auto wire=encode_workspace(state);
  const auto decoded=decode_workspace(wire);
  check(view_options_equal(decoded.default_view_options,second) &&
        view_options_equal(decoded.slots[0].view.options,second),
        "v5 defaults and per-source display values did not roundtrip");
  Workspace restored;restored.restore(decoded);
  check(view_options_equal(restored.default_view_options(),second) &&
        view_options_equal(restored.find(reopened)->document().view_options(),second),
        "v5 placeholder/default display restoration lost values");
  for(unsigned variant=0;variant<4;++variant) {
    auto bad=second;
    if(variant==0)bad.geometry_scale=.001f;
    if(variant==1)bad.joint_limit_scale=std::numeric_limits<float>::infinity();
    if(variant==2)bad.palette.particle_fluid[3]=1.1f;
    if(variant==3)bad.palette.profile_bar[0]=std::numeric_limits<float>::quiet_NaN();
    rejects([&]{workspace.set_default_view_options(bad);},"invalid display default accepted");
    auto corrupt=state;corrupt.default_view_options=bad;
    rejects([&]{encode_workspace(corrupt);},"invalid default display encoded");
    rejects([&]{workspace.restore(corrupt);},"invalid default display replaced workspace");
    corrupt=state;corrupt.slots[0].view.options=bad;
    rejects([&]{encode_workspace(corrupt);},"invalid source display encoded");
    check(view_options_equal(workspace.default_view_options(),second) &&
          encode_workspace(workspace.settings())==wire,
          "invalid display settings changed existing workspace");
  }
  std::cout<<"Workspace v5 display defaults/archives/live/palette validation passed\n";
}
} // namespace
int main() {
  try {
    const Bytes v4(workspace_v4_fixture.begin(),workspace_v4_fixture.end());
    const auto restored_v4=decode_workspace(v4);
    check(restored_v4.version==workspace_format_version &&
          restored_v4.slots.size()==1 &&
          restored_v4.captures[0].clip_states[0].clips[0].name=="Version 4 sealed clip" &&
          restored_v4.default_preferences.inspection.integer_hex &&
          restored_v4.slots[0].view.options.sleeping_scope==VisualizationScope::selected &&
          restored_v4.slots[0].view.options.trigger_scope==VisualizationScope::unselected &&
          restored_v4.slots[0].view.options.geometry_scale==1 &&
          restored_v4.slots[0].view.options.show_planes &&
          restored_v4.slots[0].view.preferences.rendering.face_culling==FaceCulling::none &&
          !restored_v4.slots[0].view.preferences.rendering.show_orbit_target &&
          view_options_equal(restored_v4.default_view_options,ViewOptions{}) &&
          restored_v4.slots[0].view.options.palette.active==ColorPalette{}.active,
          "actual v4 writer fixture lost clips/hex/scopes or fabricated v5 preferences");
    const Bytes v3(workspace_v3_fixture.begin(), workspace_v3_fixture.end());
    const auto restored_v3 = decode_workspace(v3);
    check(
        restored_v3.version == workspace_format_version && restored_v3.slots.size() == 1 &&
            restored_v3.slots[0].view.preferences.navigation.mode ==
                CameraNavigationMode::first_person &&
            restored_v3.slots[0].view.preferences.navigation.movement_speed ==
                123.5 &&
            restored_v3.slots[0].view.preferences.navigation.look_sensitivity ==
                .0125 &&
            restored_v3.default_preferences.navigation.movement_speed ==
                321.25 &&
            restored_v3.default_preferences.navigation.look_sensitivity ==
                .03125 &&
            restored_v3.default_preferences.rendering.bounds_scope ==
                VisualizationScope::all &&
            restored_v3.default_preferences.rendering.transparent_scope ==
                VisualizationScope::none &&
            !restored_v3.default_preferences.rendering.show_mass_frames &&
            !restored_v3.default_preferences.inspection.integer_hex &&
            !restored_v3.slots[0].view.preferences.inspection.integer_hex &&
            restored_v3.slots[0].view.options.sleeping_scope ==
                VisualizationScope::all &&
            restored_v3.slots[0].view.options.trigger_scope ==
                VisualizationScope::all &&
            restored_v3.captures[0].clip_states.empty(),
        "actual v3 writer fixture did not migrate navigation/visual defaults "
        "without fabricated clips");
    const Bytes v2(workspace_v2_fixture.begin(), workspace_v2_fixture.end());
    const auto restored_v2 = decode_workspace(v2);
    check(restored_v2.version == workspace_format_version &&
              restored_v2.slots.size() == 1 &&
              restored_v2.slots[0].view.camera.handedness ==
                  DisplayHandedness::left_handed &&
              restored_v2.slots[0].view.preferences.rendering.antialiasing ==
                  Antialiasing::msaa4x &&
              restored_v2.default_preferences.inspection.array_page_size ==
                  512 &&
              restored_v2.default_preferences.navigation.mode ==
                  CameraNavigationMode::orbit &&
              restored_v2.default_preferences.navigation.movement_speed == 10,
          "actual v2 workspace migration lost display state or navigation "
          "defaults");
    const Bytes old(std::begin(test_fixtures::workspace_v1),
                    std::end(test_fixtures::workspace_v1));
    const auto migrated = decode_workspace(old);
    check(migrated.version == workspace_format_version &&
              migrated.slots.size() == 1 &&
              migrated.slots[0].view.camera.handedness ==
                  DisplayHandedness::right_handed &&
              migrated.captures[0].bookmarks[0].camera &&
              migrated.captures[0].bookmarks[0].camera->handedness ==
                  DisplayHandedness::right_handed,
          "v1 cameras/bookmarks did not migrate to right-handed display");
    camera_equal(
        migrated.slots[0].view.camera,
        decode_workspace(encode_workspace(migrated)).slots[0].view.camera);
    check(preferences_equal(migrated.default_preferences, ViewPreferences{}) &&
              preferences_equal(migrated.slots[0].view.preferences,
                                ViewPreferences{}) &&
              migrated.captures[0].last_view &&
              preferences_equal(migrated.captures[0].last_view->preferences,
                                ViewPreferences{}),
          "v1 migration fabricated nondefault view preferences");
    ownership_and_locks();
    bookmarks_and_identity();
    rejected_identity_does_not_reserve_archive();
    bookmark_camera_collision_preserves_both_views();
    stopped_live_binding();
    preferences_and_source_identity();
    invalid_preferences_are_atomic();
    codec_and_corruption();
    file_and_strong_restore();
    clip_archives_and_pending();
    clip_live_binding_and_merge();
    clip_pending_shared_source_edits();
    global_display_defaults();
    clip_placeholder_limits_and_corruption();
    std::cout << "workspace tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
