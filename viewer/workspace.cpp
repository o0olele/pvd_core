#include "workspace.hpp"
#include "pvd/writer.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pvd::viewer {
namespace {
constexpr std::uint8_t magic[8] = {'P', 'V', 'D', 'W', 'S', 10, 13, 26};
constexpr std::size_t header_size = 28;
// Explicit field order is the v2 wire schema, independent of struct padding.
constexpr std::array<bool RenderOptions::*, 21> rendering_switches = {
    &RenderOptions::show_grid,
    &RenderOptions::show_axes,
    &RenderOptions::show_solids,
    &RenderOptions::show_wireframe,
    &RenderOptions::show_debug_points,
    &RenderOptions::show_debug_lines,
    &RenderOptions::show_debug_triangles,
    &RenderOptions::show_contacts,
    &RenderOptions::show_joint_frames,
    &RenderOptions::show_joint_limits,
    &RenderOptions::show_scene_queries,
    &RenderOptions::show_query_normals,
    &RenderOptions::show_query_volumes,
    &RenderOptions::show_bounds,
    &RenderOptions::show_linear_velocity,
    &RenderOptions::show_angular_velocity,
    &RenderOptions::show_cloth,
    &RenderOptions::show_particles,
    &RenderOptions::show_cloth_constraints,
    &RenderOptions::show_cloth_pins,
    &RenderOptions::show_particle_velocity};
void require(bool value, const char *message) {
  if (!value)
    throw WorkspaceError(message);
}
void utf8(const std::string &s, std::size_t limit, bool nonempty = false) {
  require(s.size() <= limit, "workspace string exceeds byte budget");
  require(!nonempty || !s.empty(), "workspace path/name must not be empty");
  for (std::size_t i = 0; i < s.size();) {
    auto a = static_cast<unsigned char>(s[i++]);
    require(a != 0, "workspace string contains NUL");
    if (a < 0x80)
      continue;
    unsigned n = 0;
    std::uint32_t cp = 0, min = 0;
    if (a >= 0xc2 && a <= 0xdf) {
      n = 1;
      cp = a & 31u;
      min = 0x80;
    } else if (a >= 0xe0 && a <= 0xef) {
      n = 2;
      cp = a & 15u;
      min = 0x800;
    } else if (a >= 0xf0 && a <= 0xf4) {
      n = 3;
      cp = a & 7u;
      min = 0x10000;
    } else
      throw WorkspaceError("workspace string is not valid UTF-8");
    require(n <= s.size() - i, "workspace UTF-8 string is truncated");
    while (n--) {
      auto b = static_cast<unsigned char>(s[i++]);
      require((b & 0xc0) == 0x80, "workspace UTF-8 continuation is invalid");
      cp = (cp << 6) | (b & 63u);
    }
    require(cp >= min && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff),
            "workspace UTF-8 scalar is invalid");
  }
}
void sample(SamplePoint s) {
  require(s == SamplePoint::native_frame || s == SamplePoint::after_fetch,
          "workspace sample mode is unsupported");
}
void camera(const OrbitCamera &c) {
  require(c.handedness == DisplayHandedness::right_handed ||
              c.handedness == DisplayHandedness::left_handed,
          "workspace camera handedness is unsupported");
  try {
    auto p = c.position();
    require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
            "workspace camera position exceeds finite range");
    auto m = c.view_projection(1);
    for (auto v : m.values)
      require(std::isfinite(v), "workspace camera matrix exceeds finite range");
  } catch (const WorkspaceError &) {
    throw;
  } catch (const std::exception &e) {
    throw WorkspaceError(std::string("workspace camera invalid: ") + e.what());
  }
}
bool same_camera(const std::optional<OrbitCamera> &a,
                 const std::optional<OrbitCamera> &b) {
  if (a.has_value() != b.has_value())
    return false;
  if (!a)
    return true;
  return std::tie(a->target.x, a->target.y, a->target.z, a->reference_up.x,
                  a->reference_up.y, a->reference_up.z, a->handedness, a->yaw,
                  a->pitch, a->distance, a->vertical_fov, a->near_plane,
                  a->far_plane) ==
         std::tie(b->target.x, b->target.y, b->target.z, b->reference_up.x,
                  b->reference_up.y, b->reference_up.z, b->handedness, b->yaw,
                  b->pitch, b->distance, b->vertical_fov, b->near_plane,
                  b->far_plane);
}
void view_options(const ViewOptions &v) {
  require(v.color_mode >= ColorMode::recorded_status &&
              v.color_mode <= ColorMode::uniform,
          "workspace color mode is unsupported");
  const auto valid_scope = [](VisualizationScope s) {
    return s >= VisualizationScope::none && s <= VisualizationScope::unselected;
  };
  require(valid_scope(v.sleeping_scope) && valid_scope(v.trigger_scope),
          "workspace category scope is unsupported");
}
void view(const StoredView &v) {
  sample(v.sample_point);
  camera(v.camera);
  view_options(v.options);
  require(valid_view_preferences(v.preferences),
          "workspace view preferences are invalid");
  require(v.scene_id || (v.frame_index == 0 &&
                         v.sample_point == SamplePoint::native_frame),
          "workspace view without scene has a frame/sample selection");
}
void identity(const std::optional<SourceIdentity> &i) {
  require(!i || i->algorithm == 1,
          "workspace source identity algorithm is unsupported");
}
void clip_binding(const clips::Binding &b) {
  require(b.source.algorithm == 1 && b.source.event_count != 0 &&
              b.scene.id != 0 && b.native_frame_count != 0 &&
              b.native_frame_count <= b.source.event_count &&
              b.available.first <= b.available.last &&
              b.available.last < b.native_frame_count,
          "workspace clip binding is invalid");
}
void clip_pool(const std::vector<clips::State> &states,
               const WorkspaceLimits &limits) {
  require(states.empty() || (limits.max_clips_per_catalog && limits.max_clip_name_bytes &&
                               limits.max_string_bytes),
          "workspace clip catalogs are disabled by zero limits");
  require(states.size() <= limits.max_clip_catalogs_per_capture,
          "workspace clip catalog count exceeds budget");
  std::size_t total = 0;
  for (const auto &state : states) {
    clip_binding(state.binding);
    require(state.clips.size() <= limits.max_clips_per_catalog &&
                state.clips.size() <= limits.max_clips_per_capture - total,
            "workspace clip count exceeds budget");
    total += state.clips.size();
    std::set<clips::Id> ids;
    for (const auto &clip : state.clips) {
      clip_binding(clip.binding);
      require(clip.binding.scene == state.binding.scene &&
                  clip.binding.source.event_count <=
                      state.binding.source.event_count &&
                  clip.id && clip.id < state.next_id &&
                  ids.insert(clip.id).second &&
                  clip.frames.first <= clip.frames.last &&
                  clip.frames.first >= clip.binding.available.first &&
                  clip.frames.last <= clip.binding.available.last,
              "workspace clip ID, source or range is invalid");
      utf8(clip.name,
           std::min(limits.max_string_bytes, limits.max_clip_name_bytes), true);
    }
    require(state.next_id && (!state.selected || ids.count(*state.selected)),
            "workspace clip selected/next ID is invalid");
  }
}
void valid(const WorkspaceSettings &s, const WorkspaceLimits &l) {
  require(s.version == workspace_format_version,
          "unsupported workspace version");
  require(valid_view_preferences(s.default_preferences),
          "workspace default preferences are invalid");
  require(s.slots.size() <= l.max_slots, "workspace slot count exceeds budget");
  require(s.captures.size() <= l.max_captures,
          "workspace capture-state count exceeds budget");
  require(s.recent_files.size() <= l.max_recent_files,
          "workspace recent-file count exceeds budget");
  std::set<std::string> paths;
  std::set<SlotId> ids;
  for (const auto &c : s.captures) {
    utf8(c.source_path, l.max_string_bytes, true);
    require(paths.insert(c.source_path).second,
            "workspace capture path is duplicated");
    require(c.bookmarks.size() <= l.max_bookmarks_per_capture,
            "workspace bookmark count exceeds budget");
    identity(c.identity);
    clip_pool(c.clip_states, l);
    if (c.last_view)
      view(*c.last_view);
    std::set<BookmarkId> books;
    for (const auto &b : c.bookmarks) {
      require(b.id && books.insert(b.id).second,
              "workspace bookmark ID is zero or duplicated");
      utf8(b.name, l.max_string_bytes, true);
      sample(b.sample_point);
      if (b.camera)
        camera(*b.camera);
      identity(b.identity);
    }
  }
  for (const auto &slot : s.slots) {
    require(slot.id && ids.insert(slot.id).second,
            "workspace slot ID is zero or duplicated");
    utf8(slot.title, l.max_string_bytes);
    utf8(slot.source_path, l.max_string_bytes, true);
    require(paths.count(slot.source_path) != 0,
            "workspace slot has no capture-state record");
    view(slot.view);
    identity(slot.identity);
  }
  require(!s.active_slot || ids.count(*s.active_slot) != 0,
          "workspace active slot does not exist");
  std::set<std::string> recent;
  for (const auto &path : s.recent_files) {
    utf8(path, l.max_string_bytes, true);
    require(recent.insert(path).second, "workspace recent path is duplicated");
  }
}
std::uint32_t crc32(const std::uint8_t *data, std::size_t size) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      auto n = i;
      for (unsigned b = 0; b < 8; ++b)
        n = (n >> 1) ^ ((n & 1) ? 0xedb88320u : 0);
      t[i] = n;
    }
    return t;
  }();
  std::uint32_t n = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i)
    n = table[(n ^ data[i]) & 255u] ^ (n >> 8);
  return n ^ 0xffffffffu;
}
struct Encoder {
  Bytes bytes;
  std::size_t limit;
  void room(std::size_t n) {
    require(bytes.size() <= limit && n <= limit - bytes.size(),
            "workspace encoded bytes exceed file budget");
  }
  void u(std::uint64_t n, std::size_t width) {
    room(width);
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
  }
  void str(const std::string &s) {
    require(s.size() <= UINT32_MAX, "workspace string exceeds u32 length");
    u(s.size(), 4);
    room(s.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
  void f(float n) {
    std::uint32_t b;
    std::memcpy(&b, &n, 4);
    u(b, 4);
  }
  void d(double n) {
    std::uint64_t b;
    static_assert(sizeof(n) == 8 && std::numeric_limits<double>::is_iec559);
    std::memcpy(&b, &n, 8);
    u(b, 8);
  }
  void identity(const std::optional<SourceIdentity> &i) {
    u(i.has_value(), 1);
    if (i) {
      u(i->algorithm, 4);
      u(i->event_count, 8);
      u(i->fingerprint, 8);
    }
  }
  void camera(const OrbitCamera &c) {
    f(c.target.x);
    f(c.target.y);
    f(c.target.z);
    f(c.reference_up.x);
    f(c.reference_up.y);
    f(c.reference_up.z);
    d(c.yaw);
    d(c.pitch);
    d(c.distance);
    d(c.vertical_fov);
    d(c.near_plane);
    d(c.far_plane);
    u(static_cast<unsigned>(c.handedness), 1);
  }
  void preferences(const ViewPreferences &v) {
    for (float component : v.rendering.background_color)
      f(component);
    u(static_cast<unsigned>(v.rendering.antialiasing), 1);
    for (auto member : rendering_switches)
      u(v.rendering.*member, 1);
    f(v.rendering.rigid_velocity_scale);
    f(v.rendering.particle_velocity_scale);
    f(v.rendering.contact_normal_length);
    f(v.rendering.point_size);
    u(static_cast<unsigned>(v.inspection.number_format), 1);
    u(v.inspection.float_precision, 1);
    u(v.inspection.double_precision, 1);
    u(v.inspection.array_page_size, 4);
    u(v.show_debug_text, 1);
    u(static_cast<unsigned>(v.navigation.mode), 1);
    d(v.navigation.movement_speed);
    d(v.navigation.look_sensitivity);
    u(static_cast<unsigned>(v.rendering.bounds_scope), 1);
    u(static_cast<unsigned>(v.rendering.velocity_scope), 1);
    u(static_cast<unsigned>(v.rendering.scene_query_scope), 1);
    u(static_cast<unsigned>(v.rendering.coordinate_frame_scope), 1);
    u(static_cast<unsigned>(v.rendering.transparent_scope), 1);
    u(v.rendering.show_actor_frames, 1);
    u(v.rendering.show_shape_frames, 1);
    u(v.rendering.show_mass_frames, 1);
    f(v.rendering.coordinate_frame_scale);
    f(v.rendering.transparent_opacity);
    u(v.inspection.integer_hex, 1);
  }
  void clip_binding(const clips::Binding &b) {
    u(b.source.algorithm, 4);
    u(b.source.event_count, 8);
    u(b.source.fingerprint, 8);
    u(b.scene.id, 8);
    u(b.scene.generation, 8);
    u(b.available.first, 8);
    u(b.available.last, 8);
    u(b.native_frame_count, 8);
  }
  void clip_state(const clips::State &s) {
    clip_binding(s.binding);
    u(s.next_id, 8);
    u(s.selected.has_value(), 1);
    if (s.selected)
      u(*s.selected, 8);
    u(s.clips.size(), 4);
    for (const auto &clip : s.clips) {
      u(clip.id, 8);
      str(clip.name);
      clip_binding(clip.binding);
      u(clip.frames.first, 8);
      u(clip.frames.last, 8);
    }
  }
  void view(const StoredView &v) {
    u(v.scene_id.has_value(), 1);
    if (v.scene_id)
      u(*v.scene_id, 8);
    u(static_cast<unsigned>(v.sample_point), 1);
    u(v.frame_index, 8);
    camera(v.camera);
    u(v.options.show_static, 1);
    u(v.options.show_dynamic, 1);
    u(v.options.show_kinematic, 1);
    u(v.options.show_sleeping, 1);
    u(v.options.show_trigger, 1);
    u(static_cast<unsigned>(v.options.color_mode), 1);
    u(v.options.uniform_color.r, 1);
    u(v.options.uniform_color.g, 1);
    u(v.options.uniform_color.b, 1);
    u(v.options.uniform_color.a, 1);
    u(static_cast<unsigned>(v.options.sleeping_scope), 1);
    u(static_cast<unsigned>(v.options.trigger_scope), 1);
    preferences(v.preferences);
  }
};
struct Decoder {
  const Bytes &bytes;
  const WorkspaceLimits &limits;
  std::size_t at = 0;
  std::uint32_t version = workspace_format_version;
  std::uint64_t u(std::size_t n) {
    require(n <= bytes.size() - at, "workspace file is truncated");
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i)
      v |= std::uint64_t(bytes[at++]) << (8 * i);
    return v;
  }
  bool boolean() {
    auto b = u(1);
    require(b <= 1, "workspace boolean is invalid");
    return b != 0;
  }
  std::size_t count(std::size_t maximum, std::size_t minimum_record_bytes = 0) {
    auto n = u(4);
    require(n <= maximum, "workspace collection count exceeds budget");
    if (minimum_record_bytes)
      require(n <= (bytes.size() - at) / minimum_record_bytes,
              "workspace collection count exceeds remaining payload");
    return static_cast<std::size_t>(n);
  }
  std::string str(std::size_t maximum) {
    auto n = count(maximum);
    require(n <= bytes.size() - at, "workspace string length exceeds file");
    std::string s(reinterpret_cast<const char *>(bytes.data() + at), n);
    at += n;
    utf8(s, maximum);
    return s;
  }
  std::string str() { return str(limits.max_string_bytes); }
  std::size_t index() {
    const auto value = u(8);
    require(value <= std::numeric_limits<std::size_t>::max(),
            "workspace clip frame index exceeds platform size");
    return static_cast<std::size_t>(value);
  }
  clips::Binding clip_binding() {
    clips::Binding b;
    b.source.algorithm = static_cast<std::uint32_t>(u(4));
    b.source.event_count = u(8);
    b.source.fingerprint = u(8);
    b.scene.id = u(8);
    b.scene.generation = u(8);
    b.available.first = index();
    b.available.last = index();
    b.native_frame_count = index();
    return b;
  }
  clips::State clip_state(std::size_t remaining_clips) {
    clips::State s;
    s.binding = clip_binding();
    s.next_id = u(8);
    if (boolean())
      s.selected = u(8);
    auto n = count(std::min(remaining_clips, limits.max_clips_per_catalog), 88);
    s.clips.reserve(n);
    while (n--) {
      clips::Clip clip;
      clip.id = u(8);
      clip.name =
          str(std::min(limits.max_string_bytes, limits.max_clip_name_bytes));
      clip.binding = clip_binding();
      clip.frames.first = index();
      clip.frames.last = index();
      s.clips.push_back(std::move(clip));
    }
    return s;
  }
  float f() {
    auto bits = static_cast<std::uint32_t>(u(4));
    float n;
    std::memcpy(&n, &bits, 4);
    return n;
  }
  double d() {
    auto bits = u(8);
    double n;
    std::memcpy(&n, &bits, 8);
    return n;
  }
  std::optional<SourceIdentity> identity() {
    if (!boolean())
      return {};
    SourceIdentity i;
    i.algorithm = static_cast<std::uint32_t>(u(4));
    i.event_count = u(8);
    i.fingerprint = u(8);
    return i;
  }
  OrbitCamera camera() {
    OrbitCamera c;
    c.target = {f(), f(), f()};
    c.reference_up = {f(), f(), f()};
    c.yaw = d();
    c.pitch = d();
    c.distance = d();
    c.vertical_fov = d();
    c.near_plane = d();
    c.far_plane = d();
    if (version >= 2)
      c.handedness = static_cast<DisplayHandedness>(u(1));
    return c;
  }
  ViewPreferences preferences() {
    ViewPreferences v;
    for (auto &component : v.rendering.background_color)
      component = f();
    v.rendering.antialiasing = static_cast<Antialiasing>(u(1));
    for (auto member : rendering_switches)
      v.rendering.*member = boolean();
    v.rendering.rigid_velocity_scale = f();
    v.rendering.particle_velocity_scale = f();
    v.rendering.contact_normal_length = f();
    v.rendering.point_size = f();
    v.inspection.number_format = static_cast<NumberFormat>(u(1));
    v.inspection.float_precision = static_cast<unsigned>(u(1));
    v.inspection.double_precision = static_cast<unsigned>(u(1));
    v.inspection.array_page_size = static_cast<std::size_t>(u(4));
    v.show_debug_text = boolean();
    if (version >= 3) {
      v.navigation.mode = static_cast<CameraNavigationMode>(u(1));
      v.navigation.movement_speed = d();
      v.navigation.look_sensitivity = d();
    }
    if (version >= 4) {
      v.rendering.bounds_scope = static_cast<VisualizationScope>(u(1));
      v.rendering.velocity_scope = static_cast<VisualizationScope>(u(1));
      v.rendering.scene_query_scope = static_cast<VisualizationScope>(u(1));
      v.rendering.coordinate_frame_scope =
          static_cast<VisualizationScope>(u(1));
      v.rendering.transparent_scope = static_cast<VisualizationScope>(u(1));
      v.rendering.show_actor_frames = boolean();
      v.rendering.show_shape_frames = boolean();
      v.rendering.show_mass_frames = boolean();
      v.rendering.coordinate_frame_scale = f();
      v.rendering.transparent_opacity = f();
      v.inspection.integer_hex = boolean();
    }
    return v;
  }
  StoredView view() {
    StoredView v;
    if (boolean())
      v.scene_id = u(8);
    v.sample_point = static_cast<SamplePoint>(u(1));
    v.frame_index = u(8);
    v.camera = camera();
    v.options.show_static = boolean();
    v.options.show_dynamic = boolean();
    v.options.show_kinematic = boolean();
    v.options.show_sleeping = boolean();
    v.options.show_trigger = boolean();
    v.options.color_mode = static_cast<ColorMode>(u(1));
    v.options.uniform_color = {
        static_cast<std::uint8_t>(u(1)), static_cast<std::uint8_t>(u(1)),
        static_cast<std::uint8_t>(u(1)), static_cast<std::uint8_t>(u(1))};
    if (version >= 4) {
      v.options.sleeping_scope = static_cast<VisualizationScope>(u(1));
      v.options.trigger_scope = static_cast<VisualizationScope>(u(1));
    }
    if (version >= 2)
      v.preferences = preferences();
    return v;
  }
};
struct PrivateFile {
  std::filesystem::path path;
  int fd = -1;
  bool published = false;
  explicit PrivateFile(const std::filesystem::path &destination,
                       bool overwrite) {
    require(!destination.empty() && !destination.filename().empty(),
            "workspace destination filename is empty");
    if (!overwrite && std::filesystem::exists(destination))
      throw WorkspaceError(
          "workspace destination already exists; overwrite was not authorized");
    static std::atomic<std::uint64_t> serial{0};
    for (unsigned i = 0; i < 128; ++i) {
      path = destination;
      path += std::filesystem::path(
          ".pvdws-tmp-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(++serial));
#ifdef _WIN32
      const auto e = _wsopen_s(&fd, path.c_str(),
                               _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                               _SH_DENYNO, _S_IREAD | _S_IWRITE);
      if (e) {
        fd = -1;
        errno = e;
      }
#else
      fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
      if (fd >= 0)
        return;
      if (errno != EEXIST)
        throw std::system_error(errno, std::generic_category(),
                                "create private workspace file");
    }
    throw WorkspaceError("unable to allocate private workspace filename");
  }
  void close_noexcept() noexcept {
    if (fd < 0)
      return;
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
    fd = -1;
  }
  ~PrivateFile() {
    close_noexcept();
    if (!published) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
  void put(const Bytes &bytes) {
    std::size_t at = 0;
    while (at < bytes.size()) {
      auto amount = static_cast<unsigned>(
          std::min<std::size_t>(INT_MAX, bytes.size() - at));
#ifdef _WIN32
      auto n = _write(fd, bytes.data() + at, amount);
#else
      auto n = ::write(fd, bytes.data() + at, amount);
#endif
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw std::system_error(errno ? errno : EIO, std::generic_category(),
                                "write workspace file");
      at += static_cast<std::size_t>(n);
    }
  }
  void publish(const std::filesystem::path &destination, bool overwrite) {
#ifdef _WIN32
    if (_commit(fd))
      throw std::system_error(errno, std::generic_category(),
                              "flush workspace file");
    auto old = fd;
    fd = -1;
    if (_close(old))
      throw std::system_error(errno, std::generic_category(),
                              "close workspace file");
    if (!MoveFileExW(path.c_str(), destination.c_str(),
                     MOVEFILE_WRITE_THROUGH |
                         (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(), "publish workspace file");
#else
    if (::fsync(fd))
      throw std::system_error(errno, std::generic_category(),
                              "flush workspace file");
    auto old = fd;
    fd = -1;
    if (::close(old))
      throw std::system_error(errno, std::generic_category(),
                              "close workspace file");
    if (overwrite) {
      if (::rename(path.c_str(), destination.c_str()))
        throw std::system_error(errno, std::generic_category(),
                                "publish workspace file");
    } else {
      if (::link(path.c_str(), destination.c_str()))
        throw std::system_error(errno, std::generic_category(),
                                "publish workspace without overwrite");
      ::unlink(path.c_str());
    }
#endif
    published = true;
  }
};
std::string title(const std::string &path) {
  auto p = path.find_last_of("/\\");
  return path.empty() ? "Untitled"
                      : path.substr(p == std::string::npos ? 0 : p + 1);
}
struct CaptureData {
  std::vector<Bookmark> bookmarks;
  std::optional<StoredView> last_view;
  std::optional<SourceIdentity> identity;
  std::vector<clips::State> clip_states;
};
clips::Limits clip_limits(const WorkspaceLimits &limits) {
  clips::Limits result;
  result.max_clips = limits.max_clips_per_catalog;
  result.max_name_bytes =
      std::min(limits.max_clip_name_bytes, limits.max_string_bytes);
  result.max_catalogs = limits.max_clip_catalogs_per_capture;
  return result;
}
bool same_clip_binding(const clips::Binding &a, const clips::Binding &b) {
  return a.source == b.source && a.scene == b.scene &&
         a.available.first == b.available.first &&
         a.available.last == b.available.last &&
         a.native_frame_count == b.native_frame_count;
}
bool same_clip(const clips::Clip &a, const clips::Clip &b) {
  return a.id == b.id && a.name == b.name &&
         same_clip_binding(a.binding, b.binding) &&
         a.frames.first == b.frames.first && a.frames.last == b.frames.last;
}
bool same_clip_state(const clips::State &a, const clips::State &b) {
  return same_clip_binding(a.binding, b.binding) && a.next_id == b.next_id &&
         a.selected == b.selected && a.clips.size() == b.clips.size() &&
         std::equal(a.clips.begin(), a.clips.end(), b.clips.begin(), same_clip);
}
void append_clip_states(std::vector<clips::State> &target,
                        const std::vector<clips::State> &incoming) {
  for (const auto &state : incoming)
    if (std::none_of(target.begin(), target.end(), [&](const auto &old) {
          return same_clip_state(old, state);
        }))
      target.push_back(state);
}
clips::State merge_clip_state(clips::State left, const clips::State &right,
                              const WorkspaceLimits &limits) {
  require(left.binding.scene == right.binding.scene,
          "Cannot merge unrelated clip scene lifetimes");
  if (right.binding.source.event_count > left.binding.source.event_count)
    left.binding = right.binding;
  left.next_id = std::max(left.next_id, right.next_id);
  std::map<clips::Id, clips::Id> mapped;
  for (const auto &original : right.clips) {
    auto next = original;
    const auto collision =
        std::find_if(left.clips.begin(), left.clips.end(),
                     [&](const auto &old) { return old.id == next.id; });
    if (collision != left.clips.end()) {
      if (same_clip(*collision, next)) {
        mapped[original.id] = next.id;
        continue;
      }
      require(left.next_id != UINT64_MAX,
              "Clip ID space exhausted while preserving conflicting metadata");
      next.id = left.next_id++;
    }
    require(left.clips.size() < limits.max_clips_per_catalog,
            "Compatible clip catalogs exceed the merged per-catalog budget");
    mapped[original.id] = next.id;
    left.clips.push_back(std::move(next));
  }
  if (right.selected)
    left.selected = mapped.at(*right.selected);
  return left;
}
struct ClipProjection {
  std::vector<clips::State> active, pending;
  std::map<clips::SceneKey, clips::Catalog> catalogs;
  std::set<std::size_t> active_records;
  std::vector<std::string> warnings;
};
ClipProjection project_clips(const Session *session,
                             const std::vector<clips::State> &pool,
                             const WorkspaceLimits &limits,
                             const std::set<std::size_t> &held_pending = {}) {
  clip_pool(pool, limits);
  ClipProjection projected;
  if (!session) {
    projected.pending = pool;
    return projected;
  }
  if (pool.empty())
    return projected;
  const auto results =
      clips::restore_catalogs(*session, pool, clip_limits(limits));
  std::map<clips::SceneKey, clips::State> combined;
  for (std::size_t i = 0; i < pool.size(); ++i) {
    if (held_pending.count(i)) {
      projected.pending.push_back(pool[i]);
      projected.warnings.push_back(
          "Clip catalog remains pending until explicit verification.");
      continue;
    }
    std::string error = results[i].error;
    if (results[i].catalog) {
      auto found = combined.find(pool[i].binding.scene);
      if (found == combined.end())
        combined.emplace(pool[i].binding.scene, pool[i]);
      else
        try {
          found->second = merge_clip_state(found->second, pool[i], limits);
        } catch (const WorkspaceError &e) {
          error = e.what();
        }
      if (error.empty()) {
        projected.active_records.insert(i);
        continue;
      }
    }
    projected.pending.push_back(pool[i]);
    projected.warnings.push_back(
        "Clip catalog scene " + std::to_string(pool[i].binding.scene.id) +
        "/generation " + std::to_string(pool[i].binding.scene.generation) +
        " remains pending: " + error);
  }
  for (auto &entry : combined)
    projected.active.push_back(std::move(entry.second));
  if (!projected.active.empty()) {
    auto restored = clips::restore_catalogs(*session, projected.active,
                                            clip_limits(limits));
    for (std::size_t i = 0; i < restored.size(); ++i) {
      require(restored[i].catalog.has_value(),
              "Merged clip catalog failed source validation");
      projected.catalogs.emplace(projected.active[i].binding.scene,
                                 std::move(*restored[i].catalog));
    }
  }
  return projected;
}
bool can_apply(const Document &doc, const StoredView &v) {
  if (!doc.loaded())
    return true;
  if (!v.scene_id)
    return v.frame_index == 0;
  for (const auto &scene : doc.scenes())
    if (scene.key.id == *v.scene_id) {
      auto count = v.sample_point == SamplePoint::native_frame
                       ? scene.native_frames
                       : scene.after_fetch_frames;
      if (v.sample_point == SamplePoint::after_fetch && !count)
        return false;
      return count ? v.frame_index < count : v.frame_index == 0;
    }
  return false;
}
bool apply(Document &doc, const StoredView &v) {
  if (!can_apply(doc, v))
    return false;
  doc.set_view_options(v.options);
  if (!doc.loaded())
    return true;
  if (v.scene_id) {
    if (!doc.set_scene(*v.scene_id) || !doc.set_sample_point(v.sample_point))
      return false;
    if (!doc.timeline().empty() &&
        !doc.set_frame(static_cast<std::size_t>(v.frame_index)))
      return false;
  }
  return true;
}
} // namespace
Bytes encode_workspace(const WorkspaceSettings &s,
                       const WorkspaceLimits &limits) {
  valid(s, limits);
  require(limits.max_file_bytes >= header_size,
          "workspace file budget is smaller than header");
  Encoder p{{}, limits.max_file_bytes - header_size};
  p.u(s.active_slot.value_or(0), 8);
  p.u(s.slots.size(), 4);
  for (const auto &slot : s.slots) {
    p.u(slot.id, 8);
    p.str(slot.title);
    p.str(slot.source_path);
    p.view(slot.view);
    p.identity(slot.identity);
  }
  p.u(s.captures.size(), 4);
  for (const auto &c : s.captures) {
    p.str(c.source_path);
    p.u(c.last_view.has_value(), 1);
    if (c.last_view)
      p.view(*c.last_view);
    p.identity(c.identity);
    p.u(c.bookmarks.size(), 4);
    for (const auto &b : c.bookmarks) {
      p.u(b.id, 8);
      p.str(b.name);
      p.u(b.scene_id, 8);
      p.u(b.frame_index, 8);
      p.u(static_cast<unsigned>(b.sample_point), 1);
      p.u(b.camera.has_value(), 1);
      if (b.camera)
        p.camera(*b.camera);
      p.identity(b.identity);
    }
    p.u(c.clip_states.size(), 4);
    for (const auto &state : c.clip_states)
      p.clip_state(state);
  }
  p.u(s.recent_files.size(), 4);
  for (const auto &r : s.recent_files)
    p.str(r);
  p.preferences(s.default_preferences);
  Encoder out{{}, limits.max_file_bytes};
  out.bytes.insert(out.bytes.end(), std::begin(magic), std::end(magic));
  out.u(s.version, 4);
  out.u(p.bytes.size(), 8);
  out.u(crc32(p.bytes.data(), p.bytes.size()), 4);
  out.u(0, 4);
  out.bytes.insert(out.bytes.end(), p.bytes.begin(), p.bytes.end());
  return std::move(out.bytes);
}
WorkspaceSettings decode_workspace(const Bytes &bytes,
                                   const WorkspaceLimits &limits) {
  require(bytes.size() <= limits.max_file_bytes,
          "workspace input exceeds file budget");
  require(bytes.size() >= header_size, "workspace header is truncated");
  require(std::equal(std::begin(magic), std::end(magic), bytes.begin()),
          "workspace magic is invalid");
  Decoder h{bytes, limits, 8};
  auto version = h.u(4);
  require(version >= 1 && version <= workspace_format_version,
          "unsupported workspace version");
  auto size = h.u(8);
  auto checksum = h.u(4);
  require(h.u(4) == 0, "workspace header reserved field is unsupported");
  require(size == bytes.size() - header_size,
          "workspace payload length is invalid");
  require(checksum ==
              crc32(bytes.data() + header_size, bytes.size() - header_size),
          "workspace checksum does not match");
  Decoder p{bytes, limits, header_size, static_cast<std::uint32_t>(version)};
  WorkspaceSettings s;
  s.version = workspace_format_version;
  auto active = p.u(8);
  if (active)
    s.active_slot = active;
  auto count = p.count(limits.max_slots, 109);
  s.slots.reserve(count);
  while (count--) {
    SlotSettings slot;
    slot.id = p.u(8);
    slot.title = p.str();
    slot.source_path = p.str();
    slot.view = p.view();
    slot.identity = p.identity();
    s.slots.push_back(std::move(slot));
  }
  count = p.count(limits.max_captures, 10);
  s.captures.reserve(count);
  while (count--) {
    CaptureSettings c;
    c.source_path = p.str();
    if (p.boolean())
      c.last_view = p.view();
    c.identity = p.identity();
    auto n = p.count(limits.max_bookmarks_per_capture, 31);
    c.bookmarks.reserve(n);
    while (n--) {
      Bookmark b;
      b.id = p.u(8);
      b.name = p.str();
      b.scene_id = p.u(8);
      b.frame_index = p.u(8);
      b.sample_point = static_cast<SamplePoint>(p.u(1));
      if (p.boolean())
        b.camera = p.camera();
      b.identity = p.identity();
      c.bookmarks.push_back(std::move(b));
    }
    if (version >= 4) {
      auto n_clips = p.count(limits.max_clip_catalogs_per_capture, 73);
      c.clip_states.reserve(n_clips);
      std::size_t total_clips = 0;
      while (n_clips--) {
        auto state = p.clip_state(limits.max_clips_per_capture - total_clips);
        total_clips += state.clips.size();
        c.clip_states.push_back(std::move(state));
      }
    }
    s.captures.push_back(std::move(c));
  }
  count = p.count(limits.max_recent_files, 4);
  s.recent_files.reserve(count);
  while (count--)
    s.recent_files.push_back(p.str());
  if (version >= 2)
    s.default_preferences = p.preferences();
  require(p.at == bytes.size(), "workspace file contains trailing payload");
  valid(s, limits);
  return s;
}
WorkspaceSettings load_workspace_file(const std::string &path,
                                      const WorkspaceLimits &limits) {
  utf8(path, limits.max_string_bytes, true);
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  if (!in)
    throw WorkspaceError("unable to open workspace file");
  Bytes bytes;
  std::array<char, 65536> block{};
  while (in) {
    in.read(block.data(), block.size());
    auto n = static_cast<std::size_t>(in.gcount());
    require(n <= limits.max_file_bytes - bytes.size(),
            "workspace input exceeds file budget");
    bytes.insert(bytes.end(), block.begin(),
                 block.begin() + static_cast<std::ptrdiff_t>(n));
  }
  require(in.eof(), "unable to read workspace file");
  return decode_workspace(bytes, limits);
}
void save_workspace_file(const std::string &path, const WorkspaceSettings &s,
                         bool overwrite, const WorkspaceLimits &limits) {
  utf8(path, limits.max_string_bytes, true);
  auto bytes = encode_workspace(s, limits);
  auto destination = std::filesystem::u8path(path);
  PrivateFile temporary(destination, overwrite);
  temporary.put(bytes);
  temporary.publish(destination, overwrite);
}
struct WorkspaceSlot::Impl {
  SlotId id = 0;
  std::string title, path;
  std::unique_ptr<Document> document;
  OrbitCamera camera;
  ViewPreferences preferences;
  bool camera_ready = false;
  std::optional<SlotLock> lock;
  std::shared_ptr<CaptureData> capture;
  std::optional<StoredView> pending_view;
  std::optional<SourceIdentity> identity, pending_identity;
  std::string warning;
  ClipProjection clips;
  std::string combined_warning;
  std::uint64_t clip_revision = 0;
  static std::string combined(const std::string &base,
                              const ClipProjection &projection) {
    auto result = base;
    if (!projection.pending.empty()) {
      if (!result.empty())
        result += " ";
      result += std::to_string(projection.pending.size()) +
                " clip catalogs remain pending source verification.";
    }
    return result;
  }
  void publish_clips(ClipProjection next, std::string text) noexcept {
    clips.active.swap(next.active);
    clips.pending.swap(next.pending);
    clips.catalogs.swap(next.catalogs);
    clips.active_records.swap(next.active_records);
    clips.warnings.swap(next.warnings);
    combined_warning.swap(text);
    ++clip_revision;
  }
  void set_clips(ClipProjection next) {
    require(clip_revision != UINT64_MAX, "workspace clip revision exhausted");
    auto text = combined(warning, next);
    publish_clips(std::move(next), std::move(text));
  }
  StoredView view() const {
    auto v = pending_view.value_or(StoredView{});
    if (document->loaded()) {
      v.scene_id = document->scene_id();
      v.sample_point = document->sample_point();
      v.frame_index = document->frame_index();
      v.options = document->view_options();
    }
    v.camera = camera;
    v.preferences = preferences;
    return v;
  }
};
WorkspaceSlot::WorkspaceSlot(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
WorkspaceSlot::~WorkspaceSlot() = default;
SlotId WorkspaceSlot::id() const noexcept { return impl_->id; }
const std::string &WorkspaceSlot::title() const noexcept {
  return impl_->title;
}
const std::string &WorkspaceSlot::source_path() const noexcept {
  return impl_->path;
}
Document &WorkspaceSlot::document() noexcept { return *impl_->document; }
const Document &WorkspaceSlot::document() const noexcept {
  return *impl_->document;
}
OrbitCamera &WorkspaceSlot::camera() noexcept { return impl_->camera; }
const OrbitCamera &WorkspaceSlot::camera() const noexcept {
  return impl_->camera;
}
ViewPreferences &WorkspaceSlot::preferences() noexcept {
  return impl_->preferences;
}
const ViewPreferences &WorkspaceSlot::preferences() const noexcept {
  return impl_->preferences;
}
bool WorkspaceSlot::camera_ready() const noexcept {
  return impl_->camera_ready;
}
void WorkspaceSlot::set_camera_ready(bool b) noexcept {
  impl_->camera_ready = b;
}
std::optional<SlotLock> WorkspaceSlot::lock_reason() const noexcept {
  return impl_->lock;
}
const std::vector<Bookmark> &WorkspaceSlot::bookmarks() const noexcept {
  return impl_->capture->bookmarks;
}
bool WorkspaceSlot::bookmarks_verified() const noexcept {
  if (!impl_->document->loaded())
    return false;
  if (impl_->path.empty())
    return true;
  if (!impl_->identity)
    return false;
  return std::all_of(impl_->capture->bookmarks.begin(),
                     impl_->capture->bookmarks.end(), [&](const auto &b) {
                       return b.identity && *b.identity == *impl_->identity;
                     });
}
bool WorkspaceSlot::bookmark_verified(BookmarkId id) const noexcept {
  if (!impl_->document->loaded())
    return false;
  for (const auto &b : impl_->capture->bookmarks)
    if (b.id == id)
      return impl_->path.empty() ||
             (b.identity && impl_->identity && *b.identity == *impl_->identity);
  return false;
}
std::optional<SourceIdentity> WorkspaceSlot::source_identity() const noexcept {
  return impl_->identity;
}
const std::vector<clips::State> &WorkspaceSlot::clip_states() const noexcept {
  return impl_->clips.active;
}
const std::vector<clips::State> &
WorkspaceSlot::pending_clip_states() const noexcept {
  return impl_->clips.pending;
}
const std::map<clips::SceneKey, clips::Catalog> &
WorkspaceSlot::clip_catalogs() const noexcept {
  return impl_->clips.catalogs;
}
std::uint64_t WorkspaceSlot::clip_revision() const noexcept {
  return impl_->clip_revision;
}
const std::string &WorkspaceSlot::warning() const noexcept {
  return impl_->combined_warning;
}
SourceIdentity workspace_source_identity(const Session &session) {
  return workspace_source_identity(session, {});
}
SourceIdentity workspace_source_identity(
    const Session &session,
    const std::function<void(std::size_t, std::size_t)> &checkpoint) {
  SourceIdentity result;
  result.event_count = session.event_count();
  result.fingerprint = 14695981039346656037ull;
  auto byte = [&](std::uint8_t b) {
    result.fingerprint ^= b;
    result.fingerprint *= 1099511628211ull;
  };
  std::size_t done = 0;
  auto bytes = [&](const Bytes &data) {
    for (std::size_t begin = 0; begin < data.size();) {
      if (checkpoint)
        checkpoint(done, session.event_count());
      const auto end =
          std::min(data.size(),
                   begin + std::min<std::size_t>(65536, data.size() - begin));
      while (begin < end)
        byte(data[begin++]);
    }
  };
  auto integer = [&](std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
      byte(static_cast<std::uint8_t>(v >> (8 * i)));
  };
  bytes(writing::encode_header(session.header()));
  for (const auto &event : session.events()) {
    if (checkpoint)
      checkpoint(done, session.event_count());
    integer(event.stream_id);
    integer(event.timestamp);
    bytes(writing::encode_event(event, session.header().byte_order));
    ++done;
  }
  if (checkpoint)
    checkpoint(done, session.event_count());
  return result;
}
const char *slot_lock_name(SlotLock l) noexcept {
  switch (l) {
  case SlotLock::loading:
    return "Loading";
  case SlotLock::live:
    return "Live";
  case SlotLock::exporting:
    return "Exporting";
  }
  return "Locked";
}
struct Workspace::Impl {
  ModelOptions model;
  WorkspaceLimits limits;
  std::vector<std::unique_ptr<WorkspaceSlot>> slots;
  std::map<std::string, std::shared_ptr<CaptureData>> captures;
  std::vector<std::string> recent;
  ViewPreferences default_preferences;
  std::optional<SlotId> active;
  SlotId next_slot = 1;
  BookmarkId next_bookmark = 1;
  Impl(ModelOptions m, WorkspaceLimits l) : model(m), limits(l) {}
  std::shared_ptr<CaptureData> capture(const std::string &path) {
    if (path.empty())
      return std::make_shared<CaptureData>();
    auto found = captures.find(path);
    if (found != captures.end())
      return found->second;
    require(captures.size() < limits.max_captures,
            "workspace capture-state budget exceeded");
    auto value = std::make_shared<CaptureData>();
    captures.emplace(path, value);
    return value;
  }
  WorkspaceSlot *find(SlotId id) const noexcept {
    for (const auto &s : slots)
      if (s->id() == id)
        return s.get();
    return nullptr;
  }
  void preserve(WorkspaceSlot &slot) {
    if (!slot.impl_->path.empty() && slot.impl_->identity &&
        slot.impl_->capture->identity &&
        *slot.impl_->identity == *slot.impl_->capture->identity) {
      auto current = slot.impl_->view();
      view(current);
      slot.impl_->capture->last_view = std::move(current);
    }
  }
  struct ClipUpdate {
    WorkspaceSlot::Impl *slot;
    ClipProjection projection;
    std::string warning;
  };
  std::vector<ClipUpdate>
  prepare_clips(const std::shared_ptr<CaptureData> &capture,
                const std::vector<clips::State> &pool,
                WorkspaceSlot *extra = nullptr,
                const std::string *extra_warning = nullptr,
                bool preserve_pending = false,
                const std::vector<clips::State> *explicit_active = nullptr) {
    clip_pool(pool, limits);
    std::vector<ClipUpdate> updates;
    std::map<std::tuple<std::uint32_t, std::uint64_t, std::uint64_t,
                        std::set<std::size_t>>,
             ClipProjection>
        cache;
    auto prepare = [&](WorkspaceSlot &slot) {
      require(slot.impl_->clip_revision != UINT64_MAX,
              "workspace clip revision exhausted");
      const auto &doc = slot.document();
      std::set<std::size_t> held_pending;
      if (preserve_pending)
        for (std::size_t i = 0; i < pool.size(); ++i)
          if (std::any_of(slot.impl_->clips.pending.begin(),
                          slot.impl_->clips.pending.end(),
                          [&](const auto &old) {
                            return same_clip_state(old, pool[i]);
                          }) &&
              (!explicit_active ||
               std::none_of(explicit_active->begin(), explicit_active->end(),
                            [&](const auto &active) {
                              return same_clip_state(active, pool[i]);
                            })))
            held_pending.insert(i);
      ClipProjection projection;
      if (doc.loaded() && !doc.live() && slot.impl_->identity &&
          slot.impl_->identity->event_count == doc.session()->event_count()) {
        const auto &identity = *slot.impl_->identity;
        const auto key = std::make_tuple(identity.algorithm, identity.event_count,
                                         identity.fingerprint, held_pending);
        auto found = cache.find(key);
        if (found == cache.end())
          found = cache.emplace(key, project_clips(doc.session(), pool, limits,
                                                    held_pending))
                      .first;
        projection = found->second;
      } else
        projection =
            project_clips(doc.loaded() ? doc.session() : nullptr, pool, limits,
                           held_pending);
      auto text = WorkspaceSlot::Impl::combined(extra == &slot && extra_warning
                                                    ? *extra_warning
                                                    : slot.impl_->warning,
                                                projection);
      updates.push_back(
          {slot.impl_.get(), std::move(projection), std::move(text)});
    };
    bool has_extra = false;
    for (const auto &slot : slots)
      if (slot->impl_->capture == capture) {
        prepare(*slot);
        has_extra = has_extra || slot.get() == extra;
      }
    if (extra && !has_extra)
      prepare(*extra);
    return updates;
  }
  static void publish_clips(const std::shared_ptr<CaptureData> &capture,
                            std::vector<clips::State> pool,
                            std::vector<ClipUpdate> updates) noexcept {
    capture->clip_states.swap(pool);
    for (auto &update : updates)
      update.slot->publish_clips(std::move(update.projection),
                                 std::move(update.warning));
  }
};
Workspace::Workspace(ModelOptions m, WorkspaceLimits l)
    : impl_(std::make_unique<Impl>(m, l)) {}
Workspace::~Workspace() = default;
SlotId Workspace::create_empty(std::string t) {
  return add(std::make_unique<Document>(impl_->model), {}, std::move(t));
}
SlotId Workspace::add(std::unique_ptr<Document> document, std::string path,
                      std::string t,
                      std::optional<SourceIdentity> precomputed) {
  require(bool(document), "workspace document is null");
  require(impl_->slots.size() < impl_->limits.max_slots,
          "workspace slot budget exceeded");
  require(impl_->next_slot != 0, "workspace slot ID space exhausted");
  utf8(path, impl_->limits.max_string_bytes);
  utf8(t, impl_->limits.max_string_bytes);
  identity(precomputed);
  auto p = std::make_unique<WorkspaceSlot::Impl>();
  p->id = impl_->next_slot;
  p->title = t.empty() ? title(path) : std::move(t);
  p->path = path;
  p->document = std::move(document);
  p->preferences = impl_->default_preferences;
  if (!path.empty() && p->document->loaded()) {
    if (precomputed) {
      require(precomputed->event_count == p->document->session()->event_count(),
              "precomputed source identity has a different event count");
      p->identity = precomputed;
    } else
      p->identity = workspace_source_identity(*p->document->session());
  }
  // Validate/compute the identity before reserving an archived source path.
  // A rejected precomputed identity must not exhaust the archive budget.
  p->capture = impl_->capture(path);
  if (p->identity) {
    if (!p->capture->identity && p->capture->bookmarks.empty() &&
        !p->capture->last_view)
      p->capture->identity = p->identity;
  }
  if (p->capture->last_view) {
    auto v = *p->capture->last_view;
    const bool matches = p->identity && p->capture->identity &&
                         *p->identity == *p->capture->identity;
    if ((!p->document->loaded() || matches) && p->capture->identity &&
        apply(*p->document, v)) {
      p->camera = v.camera;
      p->preferences = v.preferences;
      p->camera_ready = true;
      if (!p->document->loaded()) {
        p->pending_view = v;
        p->pending_identity = p->capture->identity;
      }
    } else
      p->warning = "Saved source identity/view is unverified or changed; saved "
                   "view was not applied.";
  }
  if (p->identity) {
    if (!p->capture->identity || *p->identity != *p->capture->identity) {
      p->capture->last_view.reset();
      p->capture->identity = p->identity;
    }
    if (std::any_of(p->capture->bookmarks.begin(), p->capture->bookmarks.end(),
                    [&](const auto &b) {
                      return !b.identity || *b.identity != *p->identity;
                    }))
      p->warning = "Capture content changed or is unverified. Old bookmarks "
                   "are retained but cannot be applied.";
  }
  p->set_clips(
      project_clips(p->document->loaded() ? p->document->session() : nullptr,
                    p->capture->clip_states, impl_->limits));
  auto id = p->id;
  auto slot = std::unique_ptr<WorkspaceSlot>(new WorkspaceSlot(std::move(p)));
  impl_->slots.push_back(std::move(slot));
  impl_->next_slot = id == UINT64_MAX ? 0 : id + 1;
  impl_->active = id;
  if (!path.empty())
    remember_recent(std::move(path));
  return id;
}
bool Workspace::replace(SlotId id, std::unique_ptr<Document> &incoming,
                        std::string path, std::vector<std::string> *warnings,
                        std::optional<SourceIdentity> precomputed) {
  auto *slot = find(id);
  if (!slot || slot->impl_->lock)
    return false;
  require(bool(incoming), "workspace replacement document is null");
  require(valid_view_preferences(slot->preferences()),
          "workspace view preferences are invalid");
  utf8(path, impl_->limits.max_string_bytes);
  identity(precomputed);
  std::optional<SourceIdentity> actual;
  if (!path.empty() && incoming->loaded()) {
    if (precomputed) {
      require(precomputed->event_count == incoming->session()->event_count(),
              "precomputed source identity has a different event count");
      actual = precomputed;
    } else
      actual = workspace_source_identity(*incoming->session());
  }
  auto data = impl_->capture(path);
  if (actual) {
    if (!data->identity && data->bookmarks.empty() && !data->last_view)
      data->identity = actual;
  }
  std::optional<StoredView> saved;
  std::optional<SourceIdentity> expected;
  if (!path.empty() && slot->impl_->path == path) {
    saved = slot->impl_->pending_view
                ? slot->impl_->pending_view
                : std::optional<StoredView>(slot->impl_->view());
    expected = slot->impl_->pending_view ? slot->impl_->pending_identity
                                         : slot->impl_->identity;
  } else if (!path.empty()) {
    saved = data->last_view;
    expected = data->identity;
  }
  OrbitCamera next_camera;
  ViewPreferences next_preferences = impl_->default_preferences;
  bool ready = false;
  std::string warning;
  std::optional<StoredView> pending;
  std::optional<SourceIdentity> pending_identity;
  if (saved) {
    const bool matches = actual && expected && *actual == *expected;
    if ((!incoming->loaded() || matches) && expected &&
        apply(*incoming, *saved)) {
      next_camera = saved->camera;
      next_preferences = saved->preferences;
      ready = true;
      if (!incoming->loaded()) {
        pending = saved;
        pending_identity = expected;
      }
    } else
      warning = "Saved source identity/view is unverified or changed; saved "
                "view was not applied.";
  }
  if (actual && std::any_of(data->bookmarks.begin(), data->bookmarks.end(),
                            [&](const auto &b) {
                              return !b.identity || *b.identity != *actual;
                            }))
    warning = "Capture content changed or is unverified. Old bookmarks are "
              "retained but cannot be applied.";
  auto projected =
      project_clips(incoming->loaded() ? incoming->session() : nullptr,
                    data->clip_states, impl_->limits);
  require(slot->impl_->clip_revision != UINT64_MAX,
          "workspace clip revision exhausted");
  auto combined_warning = WorkspaceSlot::Impl::combined(warning, projected);
  if (warnings)
    warnings->insert(warnings->end(), projected.warnings.begin(),
                     projected.warnings.end());
  if (!warning.empty() && warnings)
    warnings->push_back(warning);
  impl_->preserve(*slot);
  if (actual && (!data->identity || *actual != *data->identity)) {
    data->last_view.reset();
    data->identity = actual;
  }
  auto old_default = title(slot->impl_->path);
  if (slot->impl_->title.empty() || slot->impl_->title == old_default ||
      slot->impl_->title == "Untitled")
    slot->impl_->title = title(path);
  slot->impl_->document = std::move(incoming);
  slot->impl_->path = path;
  slot->impl_->capture = std::move(data);
  slot->impl_->camera = next_camera;
  slot->impl_->preferences = next_preferences;
  slot->impl_->camera_ready = ready;
  slot->impl_->pending_view = pending;
  slot->impl_->pending_identity = pending_identity;
  slot->impl_->identity = actual;
  slot->impl_->warning = std::move(warning);
  slot->impl_->publish_clips(std::move(projected), std::move(combined_warning));
  if (!path.empty())
    remember_recent(std::move(path));
  return true;
}
bool Workspace::bind_source(SlotId id, std::string path,
                            std::optional<SourceIdentity> precomputed,
                            std::vector<std::string> *warnings) {
  auto *slot = find(id);
  if (!slot || slot->lock_reason() || !slot->document().loaded() ||
      slot->document().live())
    return false;
  utf8(path, impl_->limits.max_string_bytes, true);
  identity(precomputed);
  auto actual = precomputed.value_or(SourceIdentity{});
  if (precomputed)
    require(precomputed->event_count ==
                slot->document().session()->event_count(),
            "precomputed source identity has a different event count");
  else
    actual = workspace_source_identity(*slot->document().session());
  auto current = slot->impl_->view();
  view(current);
  auto target = impl_->capture(path);
  auto merged = std::make_shared<CaptureData>(*target);
  auto next_bookmark = impl_->next_bookmark;
  if (target != slot->impl_->capture)
    for (const auto &original : slot->bookmarks()) {
      if (!slot->source_path().empty() &&
          (!original.identity || *original.identity != actual))
        continue;
      auto b = original;
      b.identity = actual;
      auto collision =
          std::find_if(merged->bookmarks.begin(), merged->bookmarks.end(),
                       [&](const auto &old) { return old.id == b.id; });
      if (collision != merged->bookmarks.end()) {
        if (collision->name == b.name && collision->scene_id == b.scene_id &&
            collision->frame_index == b.frame_index &&
            collision->sample_point == b.sample_point &&
            collision->identity == b.identity &&
            same_camera(collision->camera, b.camera))
          continue;
        require(next_bookmark != 0, "workspace bookmark ID space exhausted");
        b.id = next_bookmark;
        next_bookmark = next_bookmark == UINT64_MAX ? 0 : next_bookmark + 1;
      }
      require(merged->bookmarks.size() <
                  impl_->limits.max_bookmarks_per_capture,
              "workspace bookmark budget exceeded");
      merged->bookmarks.push_back(std::move(b));
    }
  if (target != slot->impl_->capture)
    append_clip_states(merged->clip_states,
                       slot->source_path().empty()
                           ? slot->impl_->capture->clip_states
                           : slot->clip_states());
  clip_pool(merged->clip_states, impl_->limits);
  std::string warning;
  if (std::any_of(
          merged->bookmarks.begin(), merged->bookmarks.end(),
          [&](const auto &b) { return !b.identity || *b.identity != actual; }))
    warning = "Older bookmarks for this path belong to different or unverified "
              "capture content; they remain unverified.";
  if (!warning.empty() && warnings)
    warnings->push_back(warning);
  auto clip_updates =
      impl_->prepare_clips(target, merged->clip_states, slot, &warning);
  if (warnings)
    for (const auto &update : clip_updates)
      if (update.slot == slot->impl_.get())
        warnings->insert(warnings->end(), update.projection.warnings.begin(),
                         update.projection.warnings.end());
  auto recent = impl_->recent;
  recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
  recent.insert(recent.begin(), path);
  if (recent.size() > impl_->limits.max_recent_files)
    recent.resize(impl_->limits.max_recent_files);
  merged->identity = actual;
  merged->last_view = current;
  auto clip_states = std::move(merged->clip_states);
  impl_->preserve(*slot);
  *target = std::move(*merged);
  slot->impl_->capture = target;
  slot->impl_->path = std::move(path);
  slot->impl_->identity = actual;
  slot->impl_->pending_view.reset();
  slot->impl_->pending_identity.reset();
  slot->impl_->warning = std::move(warning);
  impl_->recent.swap(recent);
  impl_->next_bookmark = next_bookmark;
  Impl::publish_clips(target, std::move(clip_states), std::move(clip_updates));
  return true;
}
bool Workspace::close(SlotId id) {
  for (std::size_t i = 0; i < impl_->slots.size(); ++i)
    if (impl_->slots[i]->id() == id) {
      if (impl_->slots[i]->impl_->lock)
        return false;
      impl_->preserve(*impl_->slots[i]);
      impl_->slots.erase(impl_->slots.begin() + static_cast<std::ptrdiff_t>(i));
      if (impl_->active == id) {
        if (impl_->slots.empty())
          impl_->active.reset();
        else
          impl_->active =
              impl_->slots[std::min(i, impl_->slots.size() - 1)]->id();
      }
      return true;
    }
  return false;
}
bool Workspace::activate(SlotId id) noexcept {
  if (!find(id))
    return false;
  impl_->active = id;
  return true;
}
bool Workspace::rename(SlotId id, std::string t) {
  auto *s = find(id);
  if (!s)
    return false;
  utf8(t, impl_->limits.max_string_bytes, true);
  s->impl_->title = std::move(t);
  return true;
}
bool Workspace::lock(SlotId id, SlotLock reason) {
  require(reason == SlotLock::loading || reason == SlotLock::live ||
              reason == SlotLock::exporting,
          "unknown workspace lock reason");
  auto *s = find(id);
  if (!s || s->impl_->lock)
    return false;
  s->impl_->lock = reason;
  return true;
}
bool Workspace::unlock(SlotId id, SlotLock reason) noexcept {
  auto *s = find(id);
  if (!s || s->impl_->lock != reason)
    return false;
  s->impl_->lock.reset();
  return true;
}
bool Workspace::any_locked() const noexcept {
  for (const auto &s : impl_->slots)
    if (s->lock_reason())
      return true;
  return false;
}
WorkspaceSlot *Workspace::find(SlotId id) noexcept { return impl_->find(id); }
const WorkspaceSlot *Workspace::find(SlotId id) const noexcept {
  return impl_->find(id);
}
WorkspaceSlot *Workspace::active() noexcept {
  return impl_->active ? find(*impl_->active) : nullptr;
}
const WorkspaceSlot *Workspace::active() const noexcept {
  return impl_->active ? find(*impl_->active) : nullptr;
}
std::optional<SlotId> Workspace::active_id() const noexcept {
  return impl_->active;
}
std::vector<WorkspaceSlot *> Workspace::slots() {
  std::vector<WorkspaceSlot *> result;
  for (auto &s : impl_->slots)
    result.push_back(s.get());
  return result;
}
std::vector<const WorkspaceSlot *> Workspace::slots() const {
  std::vector<const WorkspaceSlot *> result;
  for (const auto &s : impl_->slots)
    result.push_back(s.get());
  return result;
}
const std::vector<std::string> &Workspace::recent_files() const noexcept {
  return impl_->recent;
}
const ViewPreferences &Workspace::default_preferences() const noexcept {
  return impl_->default_preferences;
}
void Workspace::set_default_preferences(ViewPreferences value) {
  require(valid_view_preferences(value),
          "workspace default preferences are invalid");
  impl_->default_preferences = value;
}

void Workspace::remember_recent(std::string path) {
  utf8(path, impl_->limits.max_string_bytes, true);
  auto copy = impl_->recent;
  copy.erase(std::remove(copy.begin(), copy.end(), path), copy.end());
  copy.insert(copy.begin(), std::move(path));
  if (copy.size() > impl_->limits.max_recent_files)
    copy.resize(impl_->limits.max_recent_files);
  impl_->recent.swap(copy);
}
BookmarkId Workspace::add_bookmark(SlotId id, std::string name,
                                   bool include_camera) {
  auto *s = find(id);
  require(s && s->document().loaded(), "bookmark requires a loaded capture");
  require(s->document().scene_id().has_value() &&
              !s->document().timeline().empty(),
          "bookmark requires a scene and completed frame");
  utf8(name, impl_->limits.max_string_bytes, true);
  require(impl_->next_bookmark != 0, "workspace bookmark ID space exhausted");
  auto &books = s->impl_->capture->bookmarks;
  require(books.size() < impl_->limits.max_bookmarks_per_capture,
          "workspace bookmark budget exceeded");
  Bookmark b;
  b.id = impl_->next_bookmark;
  b.name = std::move(name);
  b.scene_id = *s->document().scene_id();
  b.sample_point = s->document().sample_point();
  b.frame_index = s->document().frame_index();
  b.identity = s->impl_->identity;
  if (include_camera) {
    camera(s->camera());
    b.camera = s->camera();
  }
  books.push_back(std::move(b));
  auto result = impl_->next_bookmark;
  impl_->next_bookmark = result == UINT64_MAX ? 0 : result + 1;
  return result;
}
bool Workspace::remove_bookmark(SlotId id, BookmarkId book) {
  auto *s = find(id);
  if (!s)
    return false;
  auto &books = s->impl_->capture->bookmarks;
  auto i = std::find_if(books.begin(), books.end(),
                        [&](const auto &b) { return b.id == book; });
  if (i == books.end())
    return false;
  books.erase(i);
  return true;
}
bool Workspace::rename_bookmark(SlotId id, BookmarkId book, std::string name) {
  auto *s = find(id);
  if (!s)
    return false;
  utf8(name, impl_->limits.max_string_bytes, true);
  for (auto &b : s->impl_->capture->bookmarks)
    if (b.id == book) {
      b.name = std::move(name);
      return true;
    }
  return false;
}
bool Workspace::jump_to_bookmark(SlotId id, BookmarkId book) {
  auto *s = find(id);
  if (!s || !s->document().loaded() || !s->bookmark_verified(book))
    return false;
  for (const auto &b : s->bookmarks())
    if (b.id == book) {
      StoredView v = s->impl_->view();
      v.scene_id = b.scene_id;
      v.sample_point = b.sample_point;
      v.frame_index = b.frame_index;
      if (b.camera)
        v.camera = *b.camera;
      if (!apply(s->document(), v))
        return false;
      if (b.camera) {
        s->camera() = *b.camera;
        s->set_camera_ready();
      }
      activate(id);
      return true;
    }
  return false;
}
bool Workspace::set_clip_states(SlotId id, std::vector<clips::State> incoming,
                                std::vector<std::string> *warnings) {
  auto *slot = find(id);
  if (!slot || (slot->lock_reason() && slot->lock_reason() != SlotLock::live))
    return false;
  clip_pool(incoming, impl_->limits);
  const auto *session =
      slot->document().loaded() ? slot->document().session() : nullptr;
  if (session && !incoming.empty()) {
    const auto verified =
        clips::restore_catalogs(*session, incoming, clip_limits(impl_->limits));
    std::set<clips::SceneKey> scenes;
    for (std::size_t i = 0; i < verified.size(); ++i) {
      require(verified[i].catalog.has_value(),
              "Cannot set clip metadata that is unverified for this source");
      require(scenes.insert(incoming[i].binding.scene).second,
              "Setter requires one active catalog per scene lifetime; use "
              "merge for imports");
    }
  }
  std::vector<clips::State> next;
  for (std::size_t i = 0; i < slot->impl_->capture->clip_states.size(); ++i)
    // Append may make an unpublished future seal verifiable. Replacing only
    // the already presented active records prevents an ordinary edit from
    // consuming that still-pending catalog without user verification.
    if (!slot->impl_->clips.active_records.count(i))
      next.push_back(slot->impl_->capture->clip_states[i]);
  append_clip_states(next, incoming);
  clip_pool(next, impl_->limits);
  auto updates = impl_->prepare_clips(slot->impl_->capture, next, nullptr,
                                      nullptr, true, &incoming);
  if (warnings)
    for (const auto &update : updates)
      if (update.slot == slot->impl_.get())
        warnings->insert(warnings->end(), update.projection.warnings.begin(),
                         update.projection.warnings.end());
  Impl::publish_clips(slot->impl_->capture, std::move(next),
                      std::move(updates));
  return true;
}
bool Workspace::merge_clip_states(SlotId id, std::vector<clips::State> incoming,
                                  std::vector<std::string> *warnings) {
  auto *slot = find(id);
  if (!slot || (slot->lock_reason() && slot->lock_reason() != SlotLock::live))
    return false;
  clip_pool(incoming, impl_->limits);
  auto next = slot->impl_->capture->clip_states;
  append_clip_states(next, incoming);
  clip_pool(next, impl_->limits);
  auto updates = impl_->prepare_clips(slot->impl_->capture, next);
  if (warnings)
    for (const auto &update : updates)
      if (update.slot == slot->impl_.get())
        warnings->insert(warnings->end(), update.projection.warnings.begin(),
                         update.projection.warnings.end());
  Impl::publish_clips(slot->impl_->capture, std::move(next),
                      std::move(updates));
  return true;
}
bool Workspace::verify_clip_states(SlotId id,
                                   std::vector<std::string> *warnings) {
  auto *slot = find(id);
  if (!slot || (slot->lock_reason() && slot->lock_reason() != SlotLock::live))
    return false;
  auto updates = impl_->prepare_clips(slot->impl_->capture,
                                      slot->impl_->capture->clip_states);
  if (warnings)
    for (const auto &update : updates)
      if (update.slot == slot->impl_.get())
        warnings->insert(warnings->end(), update.projection.warnings.begin(),
                         update.projection.warnings.end());
  for (auto &update : updates)
    update.slot->publish_clips(std::move(update.projection),
                               std::move(update.warning));
  return true;
}
bool Workspace::discard_pending_clip_states(SlotId id) {
  auto *slot = find(id);
  if (!slot || (slot->lock_reason() && slot->lock_reason() != SlotLock::live))
    return false;
  std::vector<clips::State> next;
  for (const auto &state : slot->impl_->capture->clip_states)
    if (std::none_of(slot->pending_clip_states().begin(),
                     slot->pending_clip_states().end(),
                     [&](const auto &pending) {
                       return same_clip_state(state, pending);
                     }))
      next.push_back(state);
  auto updates = impl_->prepare_clips(slot->impl_->capture, next);
  Impl::publish_clips(slot->impl_->capture, std::move(next),
                      std::move(updates));
  return true;
}
WorkspaceSettings Workspace::settings() const {
  WorkspaceSettings result;
  result.default_preferences = impl_->default_preferences;
  result.recent_files = impl_->recent;
  std::map<std::string, CaptureSettings> captures;
  for (const auto &c : impl_->captures)
    captures.emplace(c.first,
                     CaptureSettings{c.first, c.second->bookmarks,
                                     c.second->last_view, c.second->identity,
                                     c.second->clip_states});
  for (const auto &s : impl_->slots)
    if (!s->impl_->path.empty()) {
      auto v = s->impl_->view();
      result.slots.push_back({s->id(), s->title(), s->source_path(), v,
                              s->impl_->identity ? s->impl_->identity
                                                 : s->impl_->pending_identity});
      if (s->impl_->identity && s->impl_->capture->identity &&
          *s->impl_->identity == *s->impl_->capture->identity)
        captures.at(s->source_path()).last_view = v;
      if (impl_->active == s->id())
        result.active_slot = s->id();
    }
  if (!result.active_slot && !result.slots.empty())
    result.active_slot = result.slots.front().id;
  if (const auto *a = active())
    if (!a->source_path().empty() && a->impl_->identity &&
        a->impl_->capture->identity &&
        *a->impl_->identity == *a->impl_->capture->identity)
      captures.at(a->source_path()).last_view = a->impl_->view();
  for (auto &c : captures)
    result.captures.push_back(std::move(c.second));
  valid(result, impl_->limits);
  return result;
}
WorkspaceRestore Workspace::restore(const WorkspaceSettings &settings) {
  require(
      !any_locked(),
      "cannot replace workspace while a slot is loading, live, or exporting");
  valid(settings, impl_->limits);
  auto next = std::make_unique<Impl>(impl_->model, impl_->limits);
  next->default_preferences = settings.default_preferences;
  WorkspaceRestore result;
  for (const auto &c : settings.captures) {
    auto data = std::make_shared<CaptureData>();
    data->bookmarks = c.bookmarks;
    data->last_view = c.last_view;
    data->identity = c.identity;
    data->clip_states = c.clip_states;
    for (const auto &b : c.bookmarks) {
      if (b.id == UINT64_MAX)
        next->next_bookmark = 0;
      else if (next->next_bookmark)
        next->next_bookmark = std::max(next->next_bookmark, b.id + 1);
    }
    next->captures.emplace(c.source_path, std::move(data));
  }
  for (const auto &saved : settings.slots) {
    auto p = std::make_unique<WorkspaceSlot::Impl>();
    p->id = saved.id;
    p->title = saved.title;
    p->path = saved.source_path;
    p->document = std::make_unique<Document>(next->model);
    p->document->set_view_options(saved.view.options);
    p->camera = saved.view.camera;
    p->preferences = saved.view.preferences;
    p->camera_ready = true;
    p->pending_view = saved.view;
    p->pending_identity = saved.identity;
    p->capture = next->captures.at(p->path);
    p->set_clips(project_clips(nullptr, p->capture->clip_states, next->limits));
    next->slots.push_back(
        std::unique_ptr<WorkspaceSlot>(new WorkspaceSlot(std::move(p))));
    result.source_slots.push_back(saved.id);
    if (saved.id == UINT64_MAX)
      next->next_slot = 0;
    else if (next->next_slot)
      next->next_slot = std::max(next->next_slot, saved.id + 1);
  }
  next->recent = settings.recent_files;
  next->active = settings.active_slot;
  if (!next->active && !next->slots.empty())
    next->active = next->slots.front()->id();
  impl_.swap(next);
  return result;
}
void Workspace::save_file(const std::string &path, bool overwrite) const {
  save_workspace_file(path, settings(), overwrite, impl_->limits);
}
WorkspaceRestore Workspace::load_file(const std::string &path) {
  require(!any_locked(), "cannot load workspace while a slot is locked");
  auto s = load_workspace_file(path, impl_->limits);
  return restore(s);
}
} // namespace pvd::viewer
