#include "model.hpp"
#include "scene_tools.hpp"
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
  if (!finite(d(c.target)) || !finite(d(c.reference_up)) ||
      dot(d(c.reference_up), d(c.reference_up)) <= 0 || !std::isfinite(c.yaw) ||
      !std::isfinite(c.pitch) || !std::isfinite(c.distance) ||
      c.distance <= 0 || !std::isfinite(c.vertical_fov) ||
      c.vertical_fov <= 0 || c.vertical_fov >= pi)
    throw std::invalid_argument("invalid orbit camera parameters");
}
struct Basis {
  D3 eye, forward, right, up;
};
struct CameraAxes {
  D3 up, x, z;
};
CameraAxes camera_axes(const OrbitCamera &c) {
  const auto up = normal(d(c.reference_up));
  const D3 candidate = std::abs(up.z) < .99 ? D3{0, 0, 1} : D3{1, 0, 0};
  const auto z = normal(candidate - up * dot(candidate, up));
  return {up, normal(cross(up, z)), z};
}
D3 camera_offset(const OrbitCamera &c) {
  const auto axes = camera_axes(c);
  const auto cp = std::cos(c.pitch);
  return axes.x * (cp * std::sin(c.yaw)) + axes.up * std::sin(c.pitch) +
         axes.z * (cp * std::cos(c.yaw));
}
Basis basis(const OrbitCamera &c) {
  camera_valid(c);
  auto eye = d(c.target) + camera_offset(c) * c.distance;
  auto forward = normal(d(c.target) - eye);
  auto right = normal(cross(forward, camera_axes(c).up));
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
constexpr Color geometry_colors[] = {{225, 135, 75, 255},  {100, 195, 235, 255},
                                     {185, 120, 230, 255}, {150, 155, 165, 255},
                                     {100, 210, 140, 255}, {225, 185, 85, 255},
                                     {120, 175, 95, 255}};
Color display_color(const render::ShapeInstance &shape,
                    const ViewOptions &view) {
  switch (view.color_mode) {
  case ColorMode::recorded_status:
    return color(shape);
  case ColorMode::uniform:
    return view.uniform_color;
  case ColorMode::geometry:
    return geometry_colors[static_cast<std::size_t>(
        render::geometry_kind(shape.geometry))];
  case ColorMode::actor_kind:
    if (shape.actor.kinematic)
      return {70, 135, 245, 255};
    if (shape.actor.kind == render::ActorKind::rigid_static)
      return {220, 100, 100, 255};
    if (shape.actor.kind == render::ActorKind::articulation_link)
      return {65, 195, 200, 255};
    return {95, 205, 125, 255};
  case ColorMode::actor_identity: {
    std::uint64_t h = shape.scene_id;
    for (const auto v :
         {shape.scene_generation, shape.actor_id, shape.actor_generation})
      h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return {static_cast<std::uint8_t>(64 + (h & 159)),
            static_cast<std::uint8_t>(64 + ((h >> 8) & 159)),
            static_cast<std::uint8_t>(64 + ((h >> 16) & 159)), 255};
  }
  }
  throw std::invalid_argument("unknown viewer color mode");
}
void enable_categories(ViewOptions &v) {
  v.show_static = v.show_dynamic = v.show_kinematic = v.show_sleeping =
      v.show_trigger = true;
}

const deformables::ClothSurface *cloth_owner(const FrameState &frame,
                                             deformables::OwnerKey key) {
  const auto it = std::find_if(frame.deformables.cloths.begin(),
                               frame.deformables.cloths.end(),
                               [&](const auto &v) { return v.key == key; });
  return it == frame.deformables.cloths.end() ? nullptr : &*it;
}
const deformables::ParticleCloud *particle_owner(const FrameState &frame,
                                                 deformables::OwnerKey key) {
  const auto it = std::find_if(frame.deformables.particle_clouds.begin(),
                               frame.deformables.particle_clouds.end(),
                               [&](const auto &v) { return v.key == key; });
  return it == frame.deformables.particle_clouds.end() ? nullptr : &*it;
}
bool deformable_key_valid(const FrameState &frame,
                          const deformables::SelectionKey &key) {
  using Kind = deformables::ElementKind;
  const auto *cloth = cloth_owner(frame, key.object);
  const auto *cloud = particle_owner(frame, key.object);
  if (!cloth && !cloud)
    return false;
  if (key.kind == Kind::owner)
    return true;
  if (key.sample_event_count != frame.snapshot.event_count)
    return false;
  if (cloth && key.kind == Kind::cloth_vertex)
    return key.index < cloth->local_positions.size();
  if (cloth && key.kind == Kind::cloth_constraint)
    return std::any_of(
        cloth->constraints.begin(), cloth->constraints.end(),
        [&](const auto &edge) { return edge.index == key.index; });
  return cloud && key.kind == Kind::particle &&
         std::any_of(cloud->particles.begin(), cloud->particles.end(),
                     [&](const auto &p) { return p.index == key.index; });
}
std::optional<render::Bounds>
deformable_bounds(const FrameState &frame,
                  const deformables::SelectionKey &key) {
  using Kind = deformables::ElementKind;
  std::optional<render::Bounds> result;
  if (const auto *cloth = cloth_owner(frame, key.object)) {
    if (key.kind == Kind::owner && cloth->world_bounds)
      return render::Bounds{cloth->world_bounds->minimum,
                            cloth->world_bounds->maximum};
    if (key.kind == Kind::cloth_vertex &&
        key.index < cloth->world_positions.size())
      include(result, cloth->world_positions[key.index]);
    if (key.kind == Kind::cloth_constraint)
      for (const auto &edge : cloth->constraints)
        if (edge.index == key.index &&
            edge.particle0 < cloth->world_positions.size() &&
            edge.particle1 < cloth->world_positions.size()) {
          include(result, cloth->world_positions[edge.particle0]);
          include(result, cloth->world_positions[edge.particle1]);
          break;
        }
  } else if (const auto *cloud = particle_owner(frame, key.object)) {
    if (key.kind == Kind::owner && cloud->world_bounds)
      return render::Bounds{cloud->world_bounds->minimum,
                            cloud->world_bounds->maximum};
    if (key.kind == Kind::particle)
      for (const auto &particle : cloud->particles)
        if (particle.index == key.index && particle.position) {
          include(result, *particle.position);
          break;
        }
  }
  return result;
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
  const auto forward = camera_offset(*this) * -1;
  const auto right = normal(cross(forward, camera_axes(*this).up));
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

void OrbitCamera::set_look_at(const CameraState &source) {
  if (!finite(d(source.position)) || !finite(d(source.target)) ||
      !finite(d(source.up)))
    throw std::invalid_argument("recorded camera has non-finite coordinates");
  const auto offset = d(source.position) - d(source.target);
  const auto dist = std::sqrt(dot(offset, offset));
  const auto unit = normal(offset), up = normal(d(source.up));
  // Validate a meaningful roll before deriving spherical angles.
  (void)normal(cross(unit, up));
  OrbitCamera next = *this;
  next.target = source.target;
  next.reference_up = f(up);
  next.distance = dist;
  const auto axes = camera_axes(next);
  next.pitch = std::asin(std::clamp(dot(unit, up), -1.0, 1.0));
  next.yaw = std::atan2(dot(unit, axes.x), dot(unit, axes.z));
  next.near_plane = std::max(1e-8, dist * .001);
  next.far_plane = std::max(dist * 1000, dist + 1);
  (void)next.view_matrix();
  *this = next;
}

std::optional<PickHit> pick(const std::vector<DrawItem> &items,
                            const Ray &input) {
  if (!finite(d(input.origin)) || !finite(d(input.direction)) ||
      dot(d(input.direction), d(input.direction)) <= 0)
    return {};
  auto direction = normal(d(input.direction));
  std::optional<PickHit> best;
  for (const auto &item : items) {
    if (!item.visible || !item.pickable || !item.mesh)
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

std::optional<DeformablePickHit>
pick_deformable(const FrameState &frame, const Ray &input,
                const DeformablePickOptions &options) {
  if (!std::isfinite(options.world_radius) || options.world_radius < 0 ||
      !std::isfinite(options.angular_radius) || options.angular_radius < 0)
    throw std::invalid_argument(
        "deformable pick radii must be finite and nonnegative");
  if (!finite(d(input.origin)) || !finite(d(input.direction)) ||
      dot(d(input.direction), d(input.direction)) <= 0)
    return {};
  const auto direction = normal(d(input.direction));
  std::optional<DeformablePickHit> best;
  const auto visible = [&](deformables::OwnerKey key) {
    return std::any_of(
        frame.deformable_draws.begin(), frame.deformable_draws.end(),
        [&](const auto &v) { return v.key == key && v.visible; });
  };
  const auto candidate = [&](deformables::OwnerKey owner,
                             deformables::ElementKind kind, std::uint32_t index,
                             Vec3 position) {
    const auto delta = d(position) - d(input.origin);
    const auto along = dot(delta, direction);
    if (!finite(delta) || !std::isfinite(along) || along < 0 ||
        (best && along >= best->distance))
      return;
    const auto radius =
        std::max(options.world_radius, along * options.angular_radius);
    const auto perpendicular = delta - direction * along;
    if (dot(perpendicular, perpendicular) > radius * radius)
      return;
    best = DeformablePickHit{
        {owner, kind, index, frame.snapshot.event_count}, along, position};
  };
  if (options.cloth)
    for (const auto &owner : frame.deformables.cloths)
      if (owner.pickable && visible(owner.key))
        for (std::size_t i = 0; i < owner.world_positions.size(); ++i)
          candidate(owner.key, deformables::ElementKind::cloth_vertex,
                    static_cast<std::uint32_t>(i), owner.world_positions[i]);
  if (options.particles)
    for (const auto &owner : frame.deformables.particle_clouds)
      if (owner.pickable && visible(owner.key))
        for (const auto &particle : owner.particles)
          if (particle.position)
            candidate(owner.key, deformables::ElementKind::particle,
                      particle.index, *particle.position);
  return best;
}

struct Document::Impl {
  struct CacheEntry {
    render::Geometry source;
    std::shared_ptr<const render::TriangleBuffer> mesh;
    std::uint64_t used = 0;
  };
  ModelOptions options;
  ViewOptions view;
  std::set<PickingKey> hidden_shapes;
  std::set<std::pair<ObjectKey, ObjectKey>> hidden_actors;
  std::optional<PickingKey> isolated_shape;
  std::optional<std::pair<ObjectKey, ObjectKey>> isolated_actor;
  std::set<deformables::OwnerKey> hidden_deformables;
  std::optional<deformables::OwnerKey> isolated_deformable;
  std::unique_ptr<Session> session;
  render::Extractor extractor;
  std::string source;
  std::vector<SceneInfo> scenes;
  std::optional<std::uint64_t> scene;
  SamplePoint sample = SamplePoint::native_frame;
  std::vector<TimelineEntry> timeline;
  std::size_t index = 0;
  bool live_ingestion = false, follow = false;
  std::size_t indexed_lifetimes = 0, indexed_frames = 0, indexed_sections = 0;
  std::map<std::uint64_t, std::size_t> scene_offsets;
  std::map<std::uint64_t, std::vector<TimelineEntry>> native_timelines,
      after_timelines;
  std::set<std::uint64_t> named_scenes;
  Playback playback;
  double remainder = 0;
  std::shared_ptr<FrameState> frame = std::make_shared<FrameState>();
  std::optional<PickingKey> selected;
  std::optional<deformables::SelectionKey> selected_deformable;
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
  SceneInfo &ensure_scene(std::uint64_t id, std::uint64_t generation = 0) {
    auto found = scene_offsets.find(id);
    if (found != scene_offsets.end())
      return scenes.at(found->second);
    const auto offset = scenes.size();
    scenes.push_back({{id, generation}, "Scene " + std::to_string(id), 0, 0});
    scene_offsets.emplace(id, offset);
    if (!scene)
      scene = id;
    return scenes.back();
  }
  void index_new() {
    const auto &lives = session->lifetimes();
    for (; indexed_lifetimes < lives.size(); ++indexed_lifetimes) {
      const auto &life = lives[indexed_lifetimes];
      if (life.class_name == QualifiedName{"physx3", "PxScene"} ||
          session->metadata().is_a(life.class_name, {"physx3", "PxScene"}))
        ensure_scene(life.id, life.generation);
    }
    const auto &frames = session->frames();
    for (; indexed_frames < frames.size(); ++indexed_frames) {
      const auto &f = frames[indexed_frames];
      auto &info = ensure_scene(f.scene_id);
      ++info.native_frames;
      auto &entries = native_timelines[f.scene_id];
      TimelineEntry entry{f.scene_id,
                          f.stream_id,
                          entries.size(),
                          f.begin_event_count,
                          f.event_count,
                          SamplePoint::native_frame,
                          "Native frame " + std::to_string(entries.size() + 1) +
                              " (EndSection)"};
      entries.push_back(entry);
      if (scene == f.scene_id && sample == SamplePoint::native_frame)
        timeline.push_back(entry);
      if (!named_scenes.count(f.scene_id)) {
        auto snapshot = session->snapshot_at_event(f.event_count);
        if (auto object = snapshot.instance(f.scene_id))
          info.name = name(*object);
        named_scenes.insert(f.scene_id);
      }
    }
    const auto &sections = session->sections();
    for (; indexed_sections < sections.size(); ++indexed_sections) {
      const auto &s = sections[indexed_sections];
      if (s.name != "validation_after_fetch")
        continue;
      ++ensure_scene(s.section_id).after_fetch_frames;
      auto &entries = after_timelines[s.section_id];
      TimelineEntry entry{s.section_id,
                          s.stream_id,
                          entries.size(),
                          s.begin_event_count,
                          s.event_count,
                          SamplePoint::after_fetch,
                          "After fetch " + std::to_string(entries.size() + 1) +
                              " (explicit validation marker)"};
      entries.push_back(entry);
      if (scene == s.section_id && sample == SamplePoint::after_fetch)
        timeline.push_back(entry);
    }
  }
  void make_timeline() {
    timeline.clear();
    index = 0;
    remainder = 0;
    playback.playing = false;
    if (!session || !scene)
      return;
    auto &all = sample == SamplePoint::native_frame ? native_timelines
                                                    : after_timelines;
    auto found = all.find(*scene);
    if (found != all.end())
      timeline = found->second;
    if (live_ingestion && follow && !timeline.empty())
      index = timeline.size() - 1;
  }

  bool shape_visible(const render::ShapeInstance &s) const {
    if (isolated_deformable)
      return false;
    const auto key = s.picking_key();
    if (isolated_shape && !(key == *isolated_shape))
      return false;
    const auto actor = std::make_pair(key.scene, key.actor);
    if (isolated_actor && !(actor == *isolated_actor))
      return false;
    if (hidden_shapes.count(key) || hidden_actors.count(actor))
      return false;
    if (s.actor.kind == render::ActorKind::rigid_static) {
      if (!view.show_static)
        return false;
    } else if (s.actor.kinematic) {
      if (!view.show_kinematic)
        return false;
    } else if (!view.show_dynamic)
      return false;
    if (!view.show_sleeping && s.actor.sleeping == true)
      return false;
    return view.show_trigger || !s.flags.trigger;
  }
  void apply_view(FrameState &next) const {
    next.bounds.reset();
    for (const auto &p : next.debug.points)
      include(next.bounds, p.position);
    for (const auto &l : next.debug.lines) {
      include(next.bounds, l.position0);
      include(next.bounds, l.position1);
    }
    for (const auto &t : next.debug.triangles) {
      include(next.bounds, t.position0);
      include(next.bounds, t.position1);
      include(next.bounds, t.position2);
    }
    for (const auto &t : next.debug.texts)
      include(next.bounds, t.position);
    for (const auto &c : next.debug.contacts)
      include(next.bounds, c.position);
    std::map<PickingKey, const render::ShapeInstance *> shapes;
    for (const auto &shape : next.render.instances)
      shapes.emplace(shape.picking_key(), &shape);
    for (auto &draw : next.draws) {
      const auto found = shapes.find(draw.key);
      draw.visible = found != shapes.end() && shape_visible(*found->second);
      if (found != shapes.end())
        draw.color = display_color(*found->second, view);
      if (draw.visible) {
        if (found->second->world_bounds)
          include(next.bounds, *found->second->world_bounds);
        else if (auto bounds = draw_bounds(draw))
          include(next.bounds, *bounds);
      }
    }
    next.deformable_draws.clear();
    const auto add_deformable = [&](const auto &owner) {
      const bool visible =
          !hidden_deformables.count(owner.key) &&
          (!isolated_deformable || owner.key == *isolated_deformable) &&
          !isolated_shape && !isolated_actor;
      next.deformable_draws.push_back({owner.key, visible, false});
      if (visible && owner.world_bounds)
        include(next.bounds, render::Bounds{owner.world_bounds->minimum,
                                            owner.world_bounds->maximum});
    };
    for (const auto &owner : next.deformables.cloths)
      add_deformable(owner);
    for (const auto &owner : next.deformables.particle_clouds)
      add_deformable(owner);
  }
  void refresh_view() {
    auto next = std::make_shared<FrameState>(*frame);
    apply_view(*next);
    frame = std::move(next);
    update_selection();
  }
  void update_selection() {
    if (selected_deformable &&
        !deformable_key_valid(*frame, *selected_deformable))
      selected_deformable.reset();
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
    frame->deformable_selection = selected_deformable;
    for (auto &draw : frame->deformable_draws)
      draw.selected =
          selected_deformable && draw.key == selected_deformable->object;
  }
  void build_frame() {
    // A bounded import can end inside a property transaction. Never substitute
    // that uncommitted latest/EOF state for the previously published viewport.
    if (live_ingestion && session && timeline.empty() &&
        !session->at_transaction_boundary())
      return;
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
      next->label = live_ingestion ? "Live capture: waiting for a completed "
                                     "frame (safe current state)"
                    : sample == SamplePoint::after_fetch
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
    auto deformable_options = options.deformables;
    // A scene may not yet exist at this historical sample. Match the live
    // generation from the snapshot, never a cached later lifetime.
    if (scene) {
      const auto *object = next->snapshot.instance(*scene);
      if (object) {
        deformable_options.scene =
            deformables::ObjectKey{object->id, object->generation};
        next->deformables = deformables::extract(
            session->metadata(), next->snapshot, deformable_options);
      } else
        next->deformables.event_count = next->snapshot.event_count;
    } else
      next->deformables = deformables::extract(
          session->metadata(), next->snapshot, deformable_options);
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
    for (const auto &s : next->render.instances) {
      try {
        DrawItem item{triangles(s.geometry),
                      s.world_pose,
                      s.picking_key(),
                      color(s),
                      s.pickable,
                      s.selected};
        next->draws.push_back(std::move(item));
      } catch (const std::exception &e) {
        next->errors.push_back("Shape " + std::to_string(s.shape_id) + ": " +
                               e.what());
      }
    }
    apply_view(*next);
    frame = std::move(next);
    update_selection();
  }
  void initialize() {
    index_new();
    if (live_ingestion && follow && !timeline.empty())
      index = timeline.size() - 1;
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
void Document::begin_live(StreamHeader header, std::string label,
                          SessionOptions options) {
  auto next = std::make_unique<Impl>(impl_->options);
  next->session = std::make_unique<Session>(header, options);
  next->source = std::move(label);
  next->live_ingestion = true;
  next->follow = true;
  next->initialize();
  impl_.swap(next);
}
bool Document::append_live(std::vector<Event> events) {
  if (!impl_->live_ingestion || !impl_->session)
    throw std::logic_error("document has no live input session");
  if (events.empty())
    return false;
  auto previous = impl_->frame;
  impl_->session->append(std::move(events));
  impl_->index_new();
  if (!impl_->session->at_transaction_boundary())
    return false;
  if (impl_->follow) {
    if (!impl_->timeline.empty()) {
      impl_->index = impl_->timeline.size() - 1;
      const auto &marker = impl_->timeline.back();
      if (!previous->marker ||
          previous->marker->event_count != marker.event_count ||
          previous->marker->scene_id != marker.scene_id ||
          previous->marker->sample_point != marker.sample_point)
        impl_->build_frame();
    } else if (previous->snapshot.event_count != impl_->session->event_count())
      impl_->build_frame();
  }
  // Paused historical FrameState is immutable and is not rebuilt on polling.
  // timeline()/scenes() expose current counts independently of that
  // publication.
  return impl_->frame != previous;
}
void Document::finish_live() {
  if (!impl_->live_ingestion)
    return;
  impl_->session->finish();
  impl_->live_ingestion = false;
  impl_->follow = false;
}
bool Document::live() const noexcept { return impl_->live_ingestion; }
std::size_t Document::live_event_count() const noexcept {
  return impl_->session ? impl_->session->event_count() : 0;
}
bool Document::follow_live() const noexcept { return impl_->follow; }
void Document::set_follow_live(bool follow) {
  impl_->follow = follow && impl_->live_ingestion;
  if (impl_->follow) {
    impl_->playback.playing = false;
    impl_->remainder = 0;
    if (impl_->session->at_transaction_boundary()) {
      if (!impl_->timeline.empty())
        impl_->index = impl_->timeline.size() - 1;
      impl_->build_frame();
    }
  }
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
      impl_->after_timelines[id].empty())
    impl_->sample = SamplePoint::native_frame;
  impl_->selected.reset();
  impl_->selected_deformable.reset();
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
      (!impl_->scene || impl_->after_timelines[*impl_->scene].empty()))
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
  impl_->follow = false;
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
  if (p.playing)
    impl_->follow = false;
  if (impl_->timeline.empty())
    impl_->playback.playing = false;
  impl_->remainder = 0;
}
void Document::set_playing(bool p) {
  impl_->playback.playing = p && !impl_->timeline.empty();
  if (p)
    impl_->follow = false;
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
std::optional<deformables::SelectionKey>
Document::selected_deformable() const noexcept {
  return impl_->selected_deformable;
}
bool Document::select_deformable(const deformables::SelectionKey &requested) {
  auto key = requested;
  if (!deformable_key_valid(*impl_->frame, key))
    return false;
  if (key.kind == deformables::ElementKind::owner) {
    key.index = 0;
    key.sample_event_count = 0;
  }
  impl_->selected.reset();
  impl_->selected_deformable = key;
  impl_->inspected =
      ObjectKey{key.object.owner.id, key.object.owner.generation};
  impl_->frame = std::make_shared<FrameState>(*impl_->frame);
  impl_->update_selection();
  return true;
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
  impl_->selected_deformable.reset();
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
  for (const auto &draw : impl_->frame->deformable_draws)
    if (draw.key.owner.id == id &&
        draw.key.owner.generation == object->generation)
      return select_deformable(
          {draw.key, deformables::ElementKind::owner, 0, 0});
  if (impl_->selected_deformable) {
    const auto key = impl_->selected_deformable->object;
    const auto *cloth = cloth_owner(*impl_->frame, key);
    if (id != key.scene.id &&
        (!cloth || !cloth->fabric || cloth->fabric->id != id))
      impl_->selected_deformable.reset();
  }
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
  impl_->selected_deformable.reset();
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
  if (impl_->selected_deformable) {
    if (!deformable_visible(impl_->selected_deformable->object))
      return false;
    const auto box =
        deformable_bounds(*impl_->frame, *impl_->selected_deformable);
    if (!box)
      return false;
    c.fit(*box, aspect);
    return true;
  }
  if (impl_->inspected)
    for (const auto &owner : impl_->frame->deformable_draws)
      if (owner.visible && owner.key.owner.id == impl_->inspected->id &&
          owner.key.owner.generation == impl_->inspected->generation)
        return fit_deformable(owner.key, c, aspect);
  if (!impl_->selected && !impl_->inspected)
    return false;
  std::optional<render::Bounds> bound;
  for (const auto &draw : impl_->frame->draws) {
    if (!draw.visible)
      continue;
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

std::vector<ColorLegendEntry> view_legend(const ViewOptions &v) {
  switch (v.color_mode) {
  case ColorMode::recorded_status:
    return {{"Trigger", {190, 100, 220, 255}},
            {"Kinematic", {70, 135, 245, 255}},
            {"Static", {220, 100, 100, 255}},
            {"Sleeping", {230, 170, 70, 255}},
            {"Dynamic", {95, 205, 125, 255}}};
  case ColorMode::actor_kind:
    return {{"Static", {220, 100, 100, 255}},
            {"Dynamic", {95, 205, 125, 255}},
            {"Kinematic", {70, 135, 245, 255}},
            {"Articulation link", {65, 195, 200, 255}}};
  case ColorMode::geometry: {
    const char *labels[] = {"Box",         "Sphere",      "Capsule (X axis)",
                            "Plane (X=0)", "Convex mesh", "Triangle mesh",
                            "Height field"};
    std::vector<ColorLegendEntry> result;
    for (std::size_t i = 0; i < 7; ++i)
      result.push_back({labels[i], geometry_colors[i]});
    return result;
  }
  case ColorMode::uniform:
    return {{"Uniform", v.uniform_color}};
  case ColorMode::actor_identity:
    return {};
  }
  throw std::invalid_argument("unknown viewer color mode");
}
std::vector<ObjectGroup> Document::object_groups() const {
  std::map<QualifiedName, std::vector<ObjectNode>> groups;
  if (loaded())
    for (const auto &entry : impl_->frame->snapshot.instances) {
      const auto &object = entry.second;
      groups[object.class_name].push_back({{object.id, object.generation},
                                           name(object),
                                           object.class_name.str()});
    }
  std::vector<ObjectGroup> result;
  result.reserve(groups.size());
  for (auto &group : groups)
    result.push_back({group.first, std::move(group.second)});
  return result;
}
const ViewOptions &Document::view_options() const noexcept {
  return impl_->view;
}
void Document::set_view_options(ViewOptions options) {
  if (static_cast<int>(options.color_mode) < 0 ||
      static_cast<int>(options.color_mode) >
          static_cast<int>(ColorMode::uniform))
    throw std::invalid_argument("unknown viewer color mode");
  impl_->view = options;
  impl_->refresh_view();
}
bool Document::visible(const PickingKey &key) const noexcept {
  for (const auto &item : impl_->frame->draws)
    if (item.key == key)
      return item.visible;
  return false;
}
bool Document::deformable_visible(
    const deformables::OwnerKey &key) const noexcept {
  for (const auto &draw : impl_->frame->deformable_draws)
    if (draw.key == key)
      return draw.visible;
  return false;
}
void Document::set_deformable_visible(const deformables::OwnerKey &key,
                                      bool value) {
  if (value)
    impl_->hidden_deformables.erase(key);
  else {
    if (impl_->hidden_deformables.size() + impl_->hidden_shapes.size() +
                impl_->hidden_actors.size() >=
            100000 &&
        !impl_->hidden_deformables.count(key))
      throw std::length_error(
          "Viewer hide rule budget reached; Restore all to clear rules");
    impl_->hidden_deformables.insert(key);
  }
  impl_->refresh_view();
}
void Document::isolate_deformable(const deformables::OwnerKey &key) {
  if (!cloth_owner(*impl_->frame, key) && !particle_owner(*impl_->frame, key))
    return;
  impl_->isolated_deformable = key;
  impl_->isolated_shape.reset();
  impl_->isolated_actor.reset();
  impl_->hidden_deformables.erase(key);
  impl_->refresh_view();
}
bool Document::fit_deformable(const deformables::OwnerKey &key,
                              OrbitCamera &camera, double aspect) const {
  const auto box = deformable_bounds(
      *impl_->frame, {key, deformables::ElementKind::owner, 0, 0});
  if (!box)
    return false;
  camera.fit(*box, aspect);
  return true;
}
void Document::set_visible(const PickingKey &key, bool value) {
  if (value)
    impl_->hidden_shapes.erase(key);
  else {
    if (impl_->hidden_shapes.size() + impl_->hidden_actors.size() >= 100000 &&
        !impl_->hidden_shapes.count(key))
      throw std::length_error(
          "Viewer hide rule budget reached; Restore all to clear rules");
    impl_->hidden_shapes.insert(key);
  }
  impl_->refresh_view();
}
void Document::set_actor_visible(ObjectKey scene, ObjectKey actor, bool value) {
  const auto key = std::make_pair(scene, actor);
  if (value)
    impl_->hidden_actors.erase(key);
  else {
    if (impl_->hidden_shapes.size() + impl_->hidden_actors.size() >= 100000 &&
        !impl_->hidden_actors.count(key))
      throw std::length_error(
          "Viewer hide rule budget reached; Restore all to clear rules");
    impl_->hidden_actors.insert(key);
  }
  impl_->refresh_view();
}
void Document::isolate(const PickingKey &key) {
  impl_->isolated_deformable.reset();
  impl_->isolated_shape = key;
  impl_->isolated_actor.reset();
  impl_->hidden_shapes.erase(key);
  impl_->hidden_actors.erase({key.scene, key.actor});
  enable_categories(impl_->view);
  impl_->refresh_view();
}
void Document::isolate_actor(ObjectKey scene, ObjectKey actor) {
  impl_->isolated_deformable.reset();
  impl_->isolated_actor = std::make_pair(scene, actor);
  impl_->isolated_shape.reset();
  impl_->hidden_actors.erase({scene, actor});
  for (auto it = impl_->hidden_shapes.begin();
       it != impl_->hidden_shapes.end();)
    if (it->scene == scene && it->actor == actor)
      it = impl_->hidden_shapes.erase(it);
    else
      ++it;
  enable_categories(impl_->view);
  impl_->refresh_view();
}
bool Document::isolated() const noexcept {
  return impl_->isolated_shape.has_value() ||
         impl_->isolated_actor.has_value() ||
         impl_->isolated_deformable.has_value();
}
void Document::show_all() {
  impl_->hidden_deformables.clear();
  impl_->isolated_deformable.reset();
  impl_->hidden_shapes.clear();
  impl_->hidden_actors.clear();
  impl_->isolated_shape.reset();
  impl_->isolated_actor.reset();
  enable_categories(impl_->view);
  impl_->refresh_view();
}

namespace {
std::optional<Vec3> tracked_anchor(const Document &doc,
                                   const SceneToolState &state) {
  if (!doc.loaded() || !doc.frame())
    return {};
  if (state.tracked_shape)
    for (const auto &shape : doc.frame()->render.instances)
      if (shape.picking_key() == *state.tracked_shape)
        return shape.world_pose.position;
  if (state.tracked_object) {
    const auto *object =
        doc.frame()->snapshot.instance(state.tracked_object->id);
    if (object && object->generation == state.tracked_object->generation)
      if (auto *value = object->property("GlobalPose"))
        try {
          if (auto pose = value->as_transform();
              pose && finite(d(pose->position)))
            return pose->position;
        } catch (const std::exception &) {
        }
  }
  return {};
}
} // namespace
void stopTracking(SceneToolState &state) noexcept {
  state.tracked_source = nullptr;
  state.tracked_shape.reset();
  state.tracked_object.reset();
  state.tracked_anchor.reset();
  state.message.clear();
}
bool trackSelected(const Document &doc, const OrbitCamera &camera,
                   SceneToolState &state) {
  SceneToolState next = state;
  stopTracking(next);
  next.tracked_source = doc.session();
  next.document_source = doc.session();
  const auto inspected = doc.inspected_object();
  const auto selected = doc.selected_key();
  if (selected && (!inspected || !(selected->actor == *inspected)))
    next.tracked_shape = selected;
  else if (inspected)
    next.tracked_object = inspected;
  if (!next.tracking()) {
    state.message = "Select an actor or shape with a captured world pose";
    return false;
  }
  try {
    (void)camera.position();
    next.tracked_anchor = tracked_anchor(doc, next);
  } catch (const std::exception &e) {
    state.message = e.what();
    return false;
  }
  if (!next.tracked_anchor) {
    state.message = "The selected lifetime has no captured world-space pose";
    return false;
  }
  next.message = "Tracking selected lifetime";
  next.error.clear();
  state = std::move(next);
  return true;
}
bool updateTrackedCamera(const Document &doc, OrbitCamera &camera,
                         SceneToolState &state) {
  if (!state.tracking())
    return false;
  if (doc.session() != state.tracked_source || !doc.loaded()) {
    stopTracking(state);
    state.message = "Tracking stopped: capture changed";
    return false;
  }
  const auto point = tracked_anchor(doc, state);
  if (!point || !state.tracked_anchor) {
    stopTracking(state);
    state.message = "Tracking stopped: target lifetime is absent at this frame";
    return false;
  }
  try {
    const auto delta = d(*point) - d(*state.tracked_anchor);
    if (dot(delta, delta) == 0)
      return false;
    const auto target = f(d(camera.target) + delta);
    camera.target = target;
    state.tracked_anchor = point;
    return true;
  } catch (const std::exception &e) {
    stopTracking(state);
    state.error = e.what();
    return false;
  }
}
bool applyRecordedCamera(const Document &doc, const std::string &name,
                         OrbitCamera &camera, SceneToolState &state) {
  if (!doc.loaded() || !doc.frame()) {
    state.message = "Open a capture before choosing a camera";
    return false;
  }
  const auto found = doc.frame()->snapshot.cameras.find(name);
  if (found == doc.frame()->snapshot.cameras.end()) {
    state.message = "Recorded camera is absent at this frame";
    return false;
  }
  try {
    auto next = camera;
    next.set_look_at(found->second);
    camera = next;
    stopTracking(state);
    state.document_source = doc.session();
    state.recorded_camera = name;
    state.message = "Using recorded camera: " + name;
    state.error.clear();
    return true;
  } catch (const std::exception &e) {
    state.message = std::string("Invalid recorded camera: ") + e.what();
    return false;
  }
}
} // namespace pvd::viewer
