#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace pvd::viewer {
namespace {
constexpr double pi = 3.14159265358979323846;
struct D3 {
  double x = 0, y = 0, z = 0;
};
D3 d(Vec3 v) { return {v.x, v.y, v.z}; }
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
bool finite(D3 a) {
  return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
D3 normal(D3 a) {
  auto n = std::sqrt(dot(a, a));
  if (!(n > 0) || !std::isfinite(n))
    throw std::invalid_argument("invalid camera/ray direction");
  return a * (1 / n);
}
Vec3 f(D3 v) {
  Vec3 out{float(v.x), float(v.y), float(v.z)};
  if (!finite(d(out)))
    throw std::overflow_error("viewer coordinate exceeds float range");
  return out;
}
void camera_valid(const OrbitCamera &c) {
  if (!finite(d(c.target)) || !std::isfinite(c.yaw) ||
      !std::isfinite(c.pitch) || !std::isfinite(c.distance) ||
      c.distance <= 0 || !std::isfinite(c.vertical_fov) ||
      c.vertical_fov <= 0 || c.vertical_fov >= pi)
    throw std::invalid_argument("invalid orbit camera parameters");
}
struct Basis {
  D3 eye, forward, right, up;
};
Basis basis(const OrbitCamera &c) {
  camera_valid(c);
  auto cp = std::cos(c.pitch);
  D3 offset{cp * std::sin(c.yaw), std::sin(c.pitch), cp * std::cos(c.yaw)};
  auto eye = d(c.target) + offset * c.distance;
  auto forward = normal(d(c.target) - eye);
  auto right = normal(cross(forward, {0, 1, 0}));
  return {eye, forward, right, cross(right, forward)};
}
void include(std::optional<render::Bounds> &b, Vec3 p) {
  if (!b)
    b = render::Bounds{p, p};
  else {
    b->minimum = {std::min(b->minimum.x, p.x), std::min(b->minimum.y, p.y),
                  std::min(b->minimum.z, p.z)};
    b->maximum = {std::max(b->maximum.x, p.x), std::max(b->maximum.y, p.y),
                  std::max(b->maximum.z, p.z)};
  }
}
void include(std::optional<render::Bounds> &b, const render::Bounds &x) {
  include(b, x.minimum);
  include(b, x.maximum);
}
std::optional<render::Bounds> draw_bounds(const DrawItem &item) {
  std::optional<render::Bounds> b;
  if (item.mesh)
    for (auto p : item.mesh->positions)
      include(b, render::transform_point(item.world_pose, p));
  return b;
}
std::string name(const InstanceState &o) {
  for (const auto *n : {"Name", "name"})
    if (auto p = o.property(n); p && p->text && !p->text->empty())
      return *p->text;
  return o.class_name.name + " " + std::to_string(o.id);
}
std::vector<std::uint64_t> refs(const InstanceState &o, const char *p) {
  if (auto value = o.property(p); value && value->reference_ids)
    return *value->reference_ids;
  return {};
}
bool is_a(const Metadata &m, const InstanceState &o, const char *n) {
  QualifiedName q{"physx3", n};
  return o.class_name == q || m.is_a(o.class_name, q);
}
Color color(const render::ShapeInstance &s) {
  if (s.color)
    return *s.color;
  if (s.flags.trigger)
    return {190, 100, 220, 255};
  if (s.actor.kinematic)
    return {70, 135, 245, 255};
  if (s.actor.kind == render::ActorKind::rigid_static)
    return {220, 100, 100, 255};
  if (s.actor.sleeping == true)
    return {230, 170, 70, 255};
  return {95, 205, 125, 255};
}
} // namespace
Mat4 multiply(const Mat4 &a, const Mat4 &b) {
  Mat4 o;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      double v = 0;
      for (int k = 0; k < 4; ++k)
        v += double(a.values[k * 4 + r]) * b.values[c * 4 + k];
      o.values[c * 4 + r] = float(v);
    }
  return o;
}
Mat4 pose_matrix(const Transform &p) {
  Mat4 m;
  auto x = render::rotate(p.rotation, {1, 0, 0}),
       y = render::rotate(p.rotation, {0, 1, 0}),
       z = render::rotate(p.rotation, {0, 0, 1});
  m.values = {x.x, x.y, x.z, 0, y.x,          y.y,          y.z,          0,
              z.x, z.y, z.z, 0, p.position.x, p.position.y, p.position.z, 1};
  return m;
}
Vec3 OrbitCamera::position() const { return f(basis(*this).eye); }
Mat4 OrbitCamera::view_matrix() const {
  auto b = basis(*this);
  Mat4 m;
  m.values = {float(b.right.x),
              float(b.up.x),
              float(-b.forward.x),
              0,
              float(b.right.y),
              float(b.up.y),
              float(-b.forward.y),
              0,
              float(b.right.z),
              float(b.up.z),
              float(-b.forward.z),
              0,
              float(-dot(b.right, b.eye)),
              float(-dot(b.up, b.eye)),
              float(dot(b.forward, b.eye)),
              1};
  return m;
}
Mat4 OrbitCamera::projection_matrix(double aspect) const {
  camera_valid(*this);
  if (!std::isfinite(aspect) || aspect <= 0 || !std::isfinite(near_plane) ||
      !std::isfinite(far_plane) || near_plane <= 0 || far_plane <= near_plane)
    throw std::invalid_argument("invalid camera projection");
  auto y = 1 / std::tan(vertical_fov / 2);
  Mat4 m;
  m.values[0] = float(y / aspect);
  m.values[5] = float(y);
  m.values[10] = float(far_plane / (near_plane - far_plane));
  m.values[11] = -1;
  m.values[14] = float(far_plane * near_plane / (near_plane - far_plane));
  return m;
}
Mat4 OrbitCamera::view_projection(double aspect) const {
  return multiply(projection_matrix(aspect), view_matrix());
}
Ray OrbitCamera::screen_ray(double x, double y, double width,
                            double height) const {
  if (!std::isfinite(x) || !std::isfinite(y) || !(width > 0) || !(height > 0) ||
      !std::isfinite(width) || !std::isfinite(height))
    throw std::invalid_argument("invalid viewport dimensions");
  auto b = basis(*this);
  auto t = std::tan(vertical_fov / 2);
  auto ray =
      normal(b.forward + b.right * ((2 * x / width - 1) * t * width / height) +
             b.up * ((1 - 2 * y / height) * t));
  return {f(b.eye), f(ray)};
}
void OrbitCamera::orbit(double dx, double dy) {
  if (!std::isfinite(dx) || !std::isfinite(dy))
    return;
  yaw = std::remainder(yaw + dx, 2 * pi);
  pitch = std::clamp(pitch + dy, -pi / 2 + .001, pi / 2 - .001);
}
void OrbitCamera::pan(double dx, double dy, double height) {
  if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(height) ||
      height <= 0)
    return;
  auto b = basis(*this);
  auto scale = 2 * distance * std::tan(vertical_fov / 2) / height;
  target = f(d(target) + b.right * (-dx * scale) + b.up * (dy * scale));
}
void OrbitCamera::zoom(double steps) {
  if (!std::isfinite(steps))
    return;
  camera_valid(*this);
  // Preserve a conservative scene-depth extent established by fit(). A default
  // camera simply retains its existing far range. Zoom must not retain a near
  // plane that was appropriate only for the previous, more distant eye.
  const double extent = std::max({far_plane - distance, distance * .01, 1e-6});
  distance = std::clamp(
      distance * std::exp(-std::clamp(steps, -100.0, 100.0) * .15), 1e-4, 1e30);
  near_plane = std::max(1e-8, std::min(distance, extent) * .001);
  far_plane = distance + std::max(extent, distance * .01);
}
void OrbitCamera::fit(const render::Bounds &b, double aspect) {
  camera_valid(*this);
  if (!(aspect > 0) || !std::isfinite(aspect) || !finite(d(b.minimum)) ||
      !finite(d(b.maximum)) || b.minimum.x > b.maximum.x ||
      b.minimum.y > b.maximum.y || b.minimum.z > b.maximum.z)
    throw std::invalid_argument("invalid fit bounds/aspect");
  const auto center = (d(b.minimum) + d(b.maximum)) * .5;
  const auto extents = (d(b.maximum) - d(b.minimum)) * .5;
  const double radius = std::sqrt(dot(extents, extents));
  const double epsilon = std::max(1e-4, radius * 1e-6);
  const double tan_v = std::tan(vertical_fov / 2), tan_h = tan_v * aspect;
  // Compute the orientation directly, independently of the old target/distance.
  const D3 forward{-std::cos(pitch) * std::sin(yaw), -std::sin(pitch),
                   -std::cos(pitch) * std::cos(yaw)};
  const auto right = normal(cross(forward, {0, 1, 0}));
  const auto up = cross(right, forward);
  double required_distance = epsilon * 2, minimum_z = 0;
  for (float x : {b.minimum.x, b.maximum.x})
    for (float y : {b.minimum.y, b.maximum.y})
      for (float z : {b.minimum.z, b.maximum.z}) {
        const auto relative = D3{x, y, z} - center;
        const double horizontal = std::abs(dot(relative, right));
        const double vertical = std::abs(dot(relative, up));
        const double depth_offset = dot(relative, forward);
        minimum_z = std::min(minimum_z, depth_offset);
        // At eye distance D, this corner's depth is D + depth_offset.
        // A 12% lateral margin gives a limiting NDC coordinate of 1/1.12,
        // without the excessive retreat of a bounding sphere for wide scenes.
        required_distance =
            std::max(required_distance,
                     1.12 * std::max(horizontal / tan_h, vertical / tan_v) -
                         depth_offset);
      }
  required_distance = std::max(required_distance, -minimum_z + epsilon);
  target = f(center);
  distance = required_distance;
  // Small, scale-aware near clipping remains safe when orbiting around the
  // fitted center. The far range uses the sphere only for conservative
  // clipping, never for framing, and therefore covers every subsequent
  // yaw/pitch.
  near_plane =
      std::max(1e-8, std::min(distance, std::max(radius, epsilon)) * .001);
  far_plane = distance + std::max({radius * 1.5, epsilon * 10, distance * .01});
}

std::optional<PickHit> pick(const std::vector<DrawItem> &items,
                            const Ray &input) {
  if (!finite(d(input.origin)) || !finite(d(input.direction)) ||
      dot(d(input.direction), d(input.direction)) <= 0)
    return {};
  auto direction = normal(d(input.direction));
  std::optional<PickHit> best;
  for (const auto &item : items) {
    if (!item.pickable || !item.mesh)
      continue;
    auto q = item.world_pose.rotation;
    Quat inverse{-q.x, -q.y, -q.z, q.w};
    auto rotate_double = [](Quat q, D3 p) {
      D3 imaginary{q.x, q.y, q.z};
      auto t = cross(imaginary, p) * 2;
      return p + t * q.w + cross(imaginary, t);
    };
    auto origin =
        rotate_double(inverse, d(input.origin) - d(item.world_pose.position));
    auto dir = rotate_double(inverse, direction);
    const auto &mesh = *item.mesh;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
      auto ia = mesh.indices[i], ib = mesh.indices[i + 1],
           ic = mesh.indices[i + 2];
      if (ia >= mesh.positions.size() || ib >= mesh.positions.size() ||
          ic >= mesh.positions.size())
        continue;
      auto a = d(mesh.positions[ia]), e1 = d(mesh.positions[ib]) - a,
           e2 = d(mesh.positions[ic]) - a;
      auto p = cross(dir, e2);
      auto determinant = dot(e1, p);
      auto scale = std::sqrt(dot(e1, e1) * dot(e2, e2));
      if (!std::isfinite(determinant) || !(scale > 0) ||
          std::abs(determinant) <=
              scale * std::numeric_limits<double>::epsilon() * 16)
        continue;
      auto inv = 1 / determinant;
      auto tvec = origin - a;
      auto u = dot(tvec, p) * inv;
      if (u < -1e-9 || u > 1 + 1e-9)
        continue;
      auto v = dot(dir, cross(tvec, e1)) * inv;
      if (v < -1e-9 || u + v > 1 + 1e-9)
        continue;
      auto t = dot(e2, cross(tvec, e1)) * inv;
      if (t < 0 || !std::isfinite(t) || (best && t >= best->distance))
        continue;
      best = PickHit{item.key, t, f(d(input.origin) + direction * t), i / 3};
    }
  }
  return best;
}

struct Document::Impl {
  struct CacheEntry {
    render::Geometry source;
    std::shared_ptr<const render::TriangleBuffer> mesh;
    std::uint64_t used = 0;
  };
  ModelOptions options;
  std::unique_ptr<Session> session;
  render::Extractor extractor;
  std::string source;
  std::vector<SceneInfo> scenes;
  std::optional<std::uint64_t> scene;
  SamplePoint sample = SamplePoint::native_frame;
  std::vector<TimelineEntry> timeline;
  std::size_t index = 0;
  Playback playback;
  double remainder = 0;
  std::shared_ptr<FrameState> frame = std::make_shared<FrameState>();
  std::optional<PickingKey> selected;
  std::optional<ObjectKey> inspected;
  std::map<std::vector<std::uint64_t>, CacheEntry> cache;
  std::uint64_t clock = 0;
  explicit Impl(ModelOptions o) : options(o), extractor(o.extractor) {
    frame->label = "No recording";
  }
  std::vector<std::uint64_t> signature(const render::Geometry &g) {
    std::vector<std::uint64_t> key{g.index()};
    auto n = [&](float v) {
      std::uint32_t b;
      std::memcpy(&b, &v, 4);
      key.push_back(b);
    };
    auto vec = [&](Vec3 v) {
      n(v.x);
      n(v.y);
      n(v.z);
    };
    auto quat = [&](Quat q) {
      n(q.x);
      n(q.y);
      n(q.z);
      n(q.w);
    };
    std::visit(
        [&](const auto &v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, render::Box>)
            vec(v.half_extents);
          else if constexpr (std::is_same_v<T, render::Sphere>)
            n(v.radius);
          else if constexpr (std::is_same_v<T, render::Capsule>) {
            n(v.radius);
            n(v.half_height);
          } else if constexpr (std::is_same_v<T, render::ConvexMesh> ||
                               std::is_same_v<T, render::TriangleMesh>) {
            key.push_back(reinterpret_cast<std::uintptr_t>(v.mesh.get()));
            vec(v.scale.scale);
            quat(v.scale.rotation);
            key.push_back(v.mesh_flags);
          } else if constexpr (std::is_same_v<T, render::HeightField>) {
            key.push_back(reinterpret_cast<std::uintptr_t>(v.mesh.get()));
            n(v.row_scale);
            n(v.height_scale);
            n(v.column_scale);
            key.push_back(v.rows);
            key.push_back(v.columns);
            key.push_back(v.flags);
          }
        },
        g);
    return key;
  }
  std::shared_ptr<const render::TriangleBuffer>
  triangles(const render::Geometry &g) {
    auto key = signature(g);
    auto it = cache.find(key);
    if (it != cache.end()) {
      it->second.used = ++clock;
      return it->second.mesh;
    }
    auto data = std::make_shared<const render::TriangleBuffer>(
        render::tessellate(g, options.mesh));
    if (options.max_tessellation_cache_entries) {
      cache.emplace(std::move(key), CacheEntry{g, data, ++clock});
      while (cache.size() > options.max_tessellation_cache_entries) {
        auto oldest = std::min_element(cache.begin(), cache.end(),
                                       [](const auto &a, const auto &b) {
                                         return a.second.used < b.second.used;
                                       });
        cache.erase(oldest);
      }
    }
    return data;
  }
  void make_timeline() {
    timeline.clear();
    index = 0;
    remainder = 0;
    playback.playing = false;
    if (!session || !scene)
      return;
    if (sample == SamplePoint::native_frame) {
      for (const auto &f : session->frames())
        if (f.scene_id == *scene)
          timeline.push_back(
              {f.scene_id, f.stream_id, timeline.size(), f.begin_event_count,
               f.event_count, sample,
               "Native frame " + std::to_string(f.index) + " (EndSection)"});
    } else {
      for (const auto &s : session->sections())
        if (s.section_id == *scene && s.name == "validation_after_fetch")
          timeline.push_back({s.section_id, s.stream_id, timeline.size(),
                              s.begin_event_count, s.event_count, sample,
                              "After fetch " + std::to_string(timeline.size()) +
                                  " (explicit validation marker)"});
    }
  }
  void update_selection() {
    if (selected) {
      auto found = std::find_if(
          frame->render.instances.begin(), frame->render.instances.end(),
          [&](const auto &s) { return s.picking_key() == *selected; });
      if (found == frame->render.instances.end())
        selected.reset();
    }
    if (inspected) {
      auto object = frame->snapshot.instance(inspected->id);
      if (!object || object->generation != inspected->generation)
        inspected.reset();
    }
    for (auto &s : frame->render.instances)
      s.selected = selected && s.picking_key() == *selected;
    for (auto &d : frame->draws)
      d.selected = selected && d.key == *selected;
  }
  void build_frame() {
    auto next = std::make_shared<FrameState>();
    if (!session) {
      next->label = "No recording";
      frame = std::move(next);
      return;
    }
    next->frame_count = timeline.size();
    next->frame_index = index;
    if (!timeline.empty()) {
      next->marker = timeline.at(index);
      next->label = timeline[index].label;
      next->snapshot = session->snapshot_at_event(timeline[index].event_count);
    } else {
      next->label = sample == SamplePoint::after_fetch
                        ? "No explicit after-fetch markers; showing capture EOF"
                        : "No completed frames; showing capture EOF";
      next->snapshot = session->latest();
    }
    render::ExtractOptions selection;
    selection.scene_id = scene;
    if (selected)
      selection.selected.push_back(*selected);
    next->render =
        extractor.extract(session->metadata(), next->snapshot, selection);
    render::DebugRange range;
    if (next->marker) {
      range.begin_event_count = next->marker->begin_event_count;
      range.end_event_count = next->marker->event_count;
    } else
      range.end_event_count = next->snapshot.event_count;
    range.scene_id = scene;
    next->debug =
        render::extract_debug(next->snapshot, session->metadata(),
                              session->channels(), range, options.debug);
    // Fit visible debug-only captures too. Text contributes its anchor; glyph
    // dimensions and contact-normal display length are renderer preferences.
    for (const auto &point : next->debug.points)
      include(next->bounds, point.position);
    for (const auto &line : next->debug.lines) {
      include(next->bounds, line.position0);
      include(next->bounds, line.position1);
    }
    for (const auto &triangle : next->debug.triangles) {
      include(next->bounds, triangle.position0);
      include(next->bounds, triangle.position1);
      include(next->bounds, triangle.position2);
    }
    for (const auto &text : next->debug.texts)
      include(next->bounds, text.position);
    for (const auto &contact : next->debug.contacts)
      include(next->bounds, contact.position);

    for (const auto &s : next->render.instances) {
      try {
        DrawItem item{triangles(s.geometry),
                      s.world_pose,
                      s.picking_key(),
                      color(s),
                      s.pickable,
                      s.selected};
        if (s.world_bounds)
          include(next->bounds, *s.world_bounds);
        else if (auto b = draw_bounds(item))
          include(next->bounds, *b);
        next->draws.push_back(std::move(item));
      } catch (const std::exception &e) {
        next->errors.push_back("Shape " + std::to_string(s.shape_id) + ": " +
                               e.what());
      }
    }
    frame = std::move(next);
    update_selection();
  }
  void initialize() {
    std::map<std::uint64_t, SceneInfo> map;
    for (const auto &life : session->lifetimes())
      if (life.class_name == QualifiedName{"physx3", "PxScene"} ||
          session->metadata().is_a(life.class_name, {"physx3", "PxScene"}))
        map.emplace(life.id, SceneInfo{{life.id, life.generation},
                                       "Scene " + std::to_string(life.id),
                                       0,
                                       0});
    for (const auto &f : session->frames())
      ++map.try_emplace(f.scene_id,
                        SceneInfo{{f.scene_id, 0},
                                  "Scene " + std::to_string(f.scene_id),
                                  0,
                                  0})
            .first->second.native_frames;
    for (const auto &s : session->sections())
      if (s.name == "validation_after_fetch")
        ++map.try_emplace(s.section_id,
                          SceneInfo{{s.section_id, 0},
                                    "Scene " + std::to_string(s.section_id),
                                    0,
                                    0})
              .first->second.after_fetch_frames;
    for (auto &[id, s] : map) {
      for (const auto &f : session->frames())
        if (f.scene_id == id) {
          auto snapshot = session->snapshot_at_event(f.event_count);
          if (auto object = snapshot.instance(id))
            s.name = name(*object);
          break;
        }
      scenes.push_back(std::move(s));
    }
    if (!scenes.empty())
      scene = scenes.front().key.id;
    make_timeline();
    build_frame();
  }
};
Document::Document(ModelOptions o) : impl_(std::make_unique<Impl>(o)) {}
Document::~Document() = default;
Document::Document(Document &&) noexcept = default;
Document &Document::operator=(Document &&) noexcept = default;
void Document::open_file(const std::string &path) {
  // std::string is UTF-8 at the viewer boundary. Filesystem performs the native
  // wide-path conversion on Windows; the core API remains platform-neutral.
  std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot open capture: " + path);
  StreamDecoder decoder;
  std::array<std::uint8_t, 65536> bytes{};
  while (input) {
    input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (input.gcount() > 0)
      decoder.feed(bytes.data(), static_cast<std::size_t>(input.gcount()));
  }
  if (!input.eof())
    throw std::runtime_error("cannot read capture: " + path);
  decoder.finish();
  if (!decoder.header())
    throw std::invalid_argument("capture has no stream header");
  adopt(std::make_unique<Session>(*decoder.header(), decoder.take_events()),
        path);
}
void Document::adopt(std::unique_ptr<Session> s, std::string label) {
  if (!s || !s->state_available())
    throw std::invalid_argument("document requires a queryable session");
  auto next = std::make_unique<Impl>(impl_->options);
  next->session = std::move(s);
  next->source = std::move(label);
  next->initialize();
  impl_.swap(next);
}
bool Document::loaded() const noexcept { return bool(impl_->session); }
const Session *Document::session() const noexcept {
  return impl_->session.get();
}
const Metadata *Document::metadata() const {
  return impl_->session ? &impl_->session->metadata() : nullptr;
}
const std::string &Document::source_label() const noexcept {
  return impl_->source;
}
const std::vector<SceneInfo> &Document::scenes() const noexcept {
  return impl_->scenes;
}
std::optional<std::uint64_t> Document::scene_id() const noexcept {
  return impl_->scene;
}
bool Document::set_scene(std::uint64_t id) {
  if (std::none_of(impl_->scenes.begin(), impl_->scenes.end(),
                   [&](const auto &s) { return s.key.id == id; }))
    return false;
  if (impl_->scene == id)
    return true;
  impl_->scene = id;
  if (impl_->sample == SamplePoint::after_fetch &&
      std::none_of(impl_->session->sections().begin(),
                   impl_->session->sections().end(), [&](const auto &s) {
                     return s.section_id == id &&
                            s.name == "validation_after_fetch";
                   }))
    impl_->sample = SamplePoint::native_frame;
  impl_->selected.reset();
  impl_->inspected.reset();
  impl_->make_timeline();
  impl_->build_frame();
  return true;
}
SamplePoint Document::sample_point() const noexcept { return impl_->sample; }
bool Document::set_sample_point(SamplePoint s) {
  if (!loaded())
    return false;
  if (s == SamplePoint::after_fetch &&
      std::none_of(impl_->session->sections().begin(),
                   impl_->session->sections().end(), [&](const auto &x) {
                     return x.name == "validation_after_fetch" &&
                            impl_->scene == x.section_id;
                   }))
    return false;
  impl_->sample = s;
  impl_->make_timeline();
  impl_->build_frame();
  return true;
}
const std::vector<TimelineEntry> &Document::timeline() const noexcept {
  return impl_->timeline;
}
std::size_t Document::frame_index() const noexcept { return impl_->index; }
bool Document::set_frame(std::size_t i) {
  if (i >= impl_->timeline.size())
    return false;
  impl_->playback.playing = false;
  impl_->remainder = 0;
  if (i == impl_->index)
    return true;
  impl_->index = i;
  impl_->build_frame();
  return true;
}
bool Document::step(std::ptrdiff_t delta) {
  if (impl_->timeline.empty())
    return false;
  auto n = impl_->timeline.size();
  auto current = impl_->index;
  std::size_t index;
  if (delta >= 0) {
    auto amount = static_cast<std::size_t>(delta);
    index = amount > n - 1 - current ? n - 1 : current + amount;
  } else {
    auto amount = static_cast<std::size_t>(-(delta + 1)) + 1;
    index = amount > current ? 0 : current - amount;
  }
  return set_frame(index);
}
const Playback &Document::playback() const noexcept { return impl_->playback; }
void Document::set_playback(Playback p) {
  if (!std::isfinite(p.frames_per_second) || p.frames_per_second == 0 ||
      std::abs(p.frames_per_second) > 1000)
    throw std::invalid_argument(
        "playback FPS must be nonzero, finite, within [-1000,1000]");
  impl_->playback = p;
  if (impl_->timeline.empty())
    impl_->playback.playing = false;
  impl_->remainder = 0;
}
void Document::set_playing(bool p) {
  impl_->playback.playing = p && !impl_->timeline.empty();
  impl_->remainder = 0;
}
bool Document::advance(double elapsed) {
  if (!std::isfinite(elapsed) || elapsed < 0)
    throw std::invalid_argument("invalid elapsed playback time");
  if (!impl_->playback.playing || impl_->timeline.empty())
    return false;
  auto frames =
      impl_->remainder + elapsed * std::abs(impl_->playback.frames_per_second);
  if (!std::isfinite(frames))
    throw std::overflow_error("elapsed playback time overflow");
  auto whole = std::floor(frames);
  impl_->remainder = frames - whole;
  if (whole < 1)
    return false;
  auto n = impl_->timeline.size(), old = impl_->index;
  bool forward = impl_->playback.frames_per_second > 0;
  if (impl_->playback.loop) {
    auto movement = static_cast<std::size_t>(std::fmod(whole, double(n)));
    impl_->index = forward ? (old + movement) % n : (old + n - movement) % n;
  } else {
    auto available = forward ? n - 1 - old : old;
    if (whole >= double(available)) {
      impl_->index = forward ? n - 1 : 0;
      impl_->playback.playing = false;
      impl_->remainder = 0;
    } else {
      auto amount = static_cast<std::size_t>(whole);
      impl_->index = forward ? old + amount : old - amount;
    }
  }
  if (impl_->index == old)
    return false;
  impl_->build_frame();
  return true;
}
std::shared_ptr<const FrameState> Document::frame() const noexcept {
  return impl_->frame;
}
std::optional<PickingKey> Document::selected_key() const noexcept {
  return impl_->selected;
}
std::optional<ObjectKey> Document::inspected_object() const noexcept {
  return impl_->inspected;
}
const InstanceState *Document::inspected_instance() const noexcept {
  if (!impl_->inspected)
    return nullptr;
  auto o = impl_->frame->snapshot.instance(impl_->inspected->id);
  return o && o->generation == impl_->inspected->generation ? o : nullptr;
}
bool Document::select(const PickingKey &key) {
  auto it = std::find_if(impl_->frame->render.instances.begin(),
                         impl_->frame->render.instances.end(),
                         [&](const auto &s) { return s.picking_key() == key; });
  if (it == impl_->frame->render.instances.end())
    return false;
  impl_->selected = key;
  impl_->inspected = key.shape;
  impl_->frame = std::make_shared<FrameState>(*impl_->frame);
  impl_->update_selection();
  return true;
}
bool Document::inspect(std::uint64_t id) {
  auto object = impl_->frame->snapshot.instance(id);
  if (!object)
    return false;
  impl_->inspected = ObjectKey{id, object->generation};
  auto related = [&](const render::ShapeInstance &s) {
    bool value = s.actor_id == id || s.shape_id == id || s.geometry_id == id ||
                 s.scene_id == id;
    std::visit(
        [&](const auto &g) {
          using T = std::decay_t<decltype(g)>;
          if constexpr (std::is_same_v<T, render::TriangleMesh> ||
                        std::is_same_v<T, render::ConvexMesh> ||
                        std::is_same_v<T, render::HeightField>)
            value |= g.mesh && g.mesh->resource.id == id;
        },
        s.geometry);
    return value;
  };
  bool keep = false;
  if (impl_->selected)
    for (const auto &s : impl_->frame->render.instances)
      if (s.picking_key() == *impl_->selected && related(s)) {
        keep = true;
        break;
      }
  if (!keep) {
    impl_->selected.reset();
    // Inspecting a scene does not select an arbitrary shape. A related existing
    // attachment is retained so Scene/Actor/Shape role navigation stays stable.
    if (!is_a(impl_->session->metadata(), *object, "PxScene"))
      for (const auto &s : impl_->frame->render.instances)
        if (related(s)) {
          impl_->selected = s.picking_key();
          break;
        }
  }
  impl_->frame = std::make_shared<FrameState>(*impl_->frame);
  impl_->update_selection();
  return true;
}

void Document::clear_selection() {
  impl_->selected.reset();
  impl_->inspected.reset();
  impl_->frame = std::make_shared<FrameState>(*impl_->frame);
  impl_->update_selection();
}
std::vector<ActorNode> Document::actor_tree() const {
  std::vector<ActorNode> result;
  if (!loaded())
    return result;
  const auto &snapshot = impl_->frame->snapshot;
  const auto &m = impl_->session->metadata();
  for (const auto &[id, object] : snapshot.instances) {
    if (!is_a(m, object, "PxRigidActor") && !is_a(m, object, "PxRigidStatic") &&
        !is_a(m, object, "PxRigidDynamic") &&
        !is_a(m, object, "PxArticulationLink"))
      continue;
    auto scenes = refs(object, "Scene");
    bool visible =
        !impl_->scene || (!scenes.empty() && scenes.front() == *impl_->scene);
    if (!visible)
      for (const auto &s : impl_->frame->render.instances)
        if (s.actor_id == id) {
          visible = true;
          break;
        }
    if (!visible)
      continue;
    ActorNode actor{
        {id, object.generation}, name(object), object.class_name.str(), {}};
    std::set<std::uint64_t> seen;
    for (auto sid : refs(object, "Shapes")) {
      if (!seen.insert(sid).second)
        continue;
      ShapeNode shape;
      shape.key.id = sid;
      if (auto o = snapshot.instance(sid)) {
        shape.key.generation = o->generation;
        shape.name = name(*o);
        shape.class_name = o->class_name.str();
        auto geometry = refs(*o, "Geometry");
        if (!geometry.empty())
          shape.geometry_id = geometry.front();
      } else {
        shape.present = false;
        shape.name = "Missing shape " + std::to_string(sid);
      }
      actor.shapes.push_back(std::move(shape));
    }
    result.push_back(std::move(actor));
  }
  return result;
}
bool Document::fit_scene(OrbitCamera &c, double aspect) const {
  if (!impl_->frame->bounds)
    return false;
  c.fit(*impl_->frame->bounds, aspect);
  return true;
}
bool Document::fit_selected(OrbitCamera &c, double aspect) const {
  if (!impl_->selected && !impl_->inspected)
    return false;
  std::optional<render::Bounds> bound;
  for (const auto &draw : impl_->frame->draws) {
    bool match = impl_->selected && draw.key == *impl_->selected;
    if (impl_->inspected && draw.key.actor == *impl_->inspected)
      match = true;
    if (match)
      if (auto b = draw_bounds(draw))
        include(bound, *b);
  }
  if (!bound)
    return false;
  c.fit(*bound, aspect);
  return true;
}
std::size_t Document::tessellation_cache_entries() const noexcept {
  return impl_->cache.size();
}
} // namespace pvd::viewer
