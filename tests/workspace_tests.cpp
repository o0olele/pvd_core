#include "pvd/writer.hpp"
#include "workspace.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

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
            a.far_plane == b.far_plane,
        "camera persistence lost values/reference up");
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
WorkspaceSettings minimal() {
  Fixture f;
  Workspace w;
  auto id = w.add(f.document(), u8"D:/\u6355\u83b7/\U0001F680.pxd2",
                  u8"\u8c03\u8bd5 capture");
  auto *s = w.find(id);
  s->document().set_frame(2);
  s->camera().set_look_at({{5, 2, 10}, {0, 0, 1}, {0, 0, 0}});
  s->camera().distance = std::nextafter(123.0, 124.0);
  auto options = s->document().view_options();
  options.show_static = false;
  options.color_mode = ColorMode::uniform;
  options.uniform_color = {1, 2, 3, 4};
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
            decoded.slots[0].view.options.uniform_color.a == 4,
        "display options lost");
  check(encode_workspace(decoded) == bytes,
        "workspace codec not exact/UTF8 stable");
  for (std::size_t n = 0; n < bytes.size(); ++n) {
    Bytes cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
    rejects([&] { decode_workspace(cut); }, "truncated workspace accepted");
  }
  auto version = bytes;
  version[8] = 2;
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
  bad.slots[0].view.camera.reference_up = {0, 0, 0};
  rejects([&] { encode_workspace(bad); }, "invalid camera accepted");
  bad = settings;
  bad.slots[0].view.options.color_mode = static_cast<ColorMode>(99);
  rejects([&] { encode_workspace(bad); }, "unknown color mode accepted");
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
  invalid.version = 2;
  rejects([&] { save_workspace_file(file.u8string(), invalid, true); },
          "invalid overwrite accepted");
  check(read(file) == original, "failed overwrite damaged workspace");
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
} // namespace
int main() {
  try {
    ownership_and_locks();
    bookmarks_and_identity();
    stopped_live_binding();
    codec_and_corruption();
    file_and_strong_restore();
    std::cout << "workspace tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
