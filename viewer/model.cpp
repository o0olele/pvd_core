#include "model.hpp"
#include "pvd/coordinates.hpp"
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
#include <tuple>
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
class DisplayOverflow final : public std::overflow_error {
public:
  DisplayOverflow() : std::overflow_error("scaled display coordinates exceed float range") {}
};
Vec3 display_f(D3 value) {
  try { return f(value); }
  catch (const std::overflow_error &) { throw DisplayOverflow(); }
}
std::optional<D3> ray_clip_forward(const Ray &ray) {
  if (!std::isfinite(ray.min_distance) || ray.min_distance < 0 ||
      !(ray.max_distance >= ray.min_distance))
    throw std::invalid_argument("invalid ray distance interval");
  if (!ray.view_clip) return {};
  const auto &clip = *ray.view_clip;
  if (!finite(d(clip.forward)) || !std::isfinite(clip.near_depth) ||
      clip.near_depth < 0 || !(clip.far_depth >= clip.near_depth))
    throw std::invalid_argument("invalid ray view clipping policy");
  return normal(d(clip.forward));
}
std::optional<std::pair<double, double>> ray_interval(
    const Ray &ray, D3 direction, const std::optional<D3> &forward) {
  double minimum = ray.min_distance, maximum = ray.max_distance;
  if (forward) {
    const auto rate = dot(direction, *forward);
    if (rate == 0) {
      if (ray.view_clip->near_depth > 0) return {};
    } else {
      auto first = ray.view_clip->near_depth / rate;
      auto last = ray.view_clip->far_depth / rate;
      if (last < first) std::swap(first, last);
      minimum = std::max(minimum, first);
      maximum = std::min(maximum, last);
    }
  }
  if (minimum > maximum) return {};
  return std::make_pair(minimum, maximum);
}
void camera_valid(const OrbitCamera &c) {
  if (!finite(d(c.target)) || !finite(d(c.reference_up)) ||
      dot(d(c.reference_up), d(c.reference_up)) <= 0 || !std::isfinite(c.yaw) ||
      !std::isfinite(c.pitch) || !std::isfinite(c.distance) ||
      c.distance <= 0 || !std::isfinite(c.vertical_fov) ||
      c.vertical_fov <= 0 || c.vertical_fov >= pi ||
      (c.handedness != DisplayHandedness::right_handed &&
       c.handedness != DisplayHandedness::left_handed))
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
  const auto up = cross(right, forward);
  if (c.handedness == DisplayHandedness::left_handed)
    right = right * -1;
  return {eye, forward, right, up};
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
  return display_bounds(item);
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
constexpr Color geometry_colors[] = {{225, 135, 75, 255},  {100, 195, 235, 255},
                                     {185, 120, 230, 255}, {150, 155, 165, 255},
                                     {100, 210, 140, 255}, {225, 185, 85, 255},
                                     {120, 175, 95, 255}};
Color display_color(const render::ShapeInstance &shape,
                    const ViewOptions &view) {
  switch (view.color_mode) {
  case ColorMode::recorded_status:
    return status_color(shape, view.palette);
  case ColorMode::uniform:
    return view.uniform_color;
  case ColorMode::geometry:
    return geometry_colors[static_cast<std::size_t>(
        render::geometry_kind(shape.geometry))];
  case ColorMode::actor_kind:
    return actor_kind_color(shape, view.palette);
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
bool same_view_options(const ViewOptions &a, const ViewOptions &b) {
  if (std::tie(a.show_static, a.show_dynamic, a.show_kinematic, a.show_sleeping,
               a.show_trigger, a.color_mode, a.uniform_color.r, a.uniform_color.g,
               a.uniform_color.b, a.uniform_color.a, a.sleeping_scope,
               a.trigger_scope, a.geometry_scale, a.show_planes,
               a.joint_frame_scale, a.joint_limit_scale, a.palette.profile_by_event) !=
      std::tie(b.show_static, b.show_dynamic, b.show_kinematic, b.show_sleeping,
               b.show_trigger, b.color_mode, b.uniform_color.r, b.uniform_color.g,
               b.uniform_color.b, b.uniform_color.a, b.sleeping_scope,
               b.trigger_scope, b.geometry_scale, b.show_planes,
               b.joint_frame_scale, b.joint_limit_scale, b.palette.profile_by_event))
    return false;
  for (auto role : color_palette_roles)
    if (std::memcmp(palette_color(a.palette, role).data(),
                    palette_color(b.palette, role).data(), 4 * sizeof(float)))
      return false;
  // The workspace codec preserves IEEE bits, including signed zero.
  for (const auto values : {std::pair<float, float>{a.geometry_scale, b.geometry_scale},
                            {a.joint_frame_scale, b.joint_frame_scale},
                            {a.joint_limit_scale, b.joint_limit_scale}})
    if (std::memcmp(&values.first, &values.second, sizeof(float)))
      return false;
  return true;
}
void enable_categories(ViewOptions &v) {
  v.show_static = v.show_dynamic = v.show_kinematic = v.show_sleeping =
      v.show_trigger = true;
  v.sleeping_scope = v.trigger_scope = VisualizationScope::all;
  v.show_planes = true;
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

template <class SnapshotLike>
bool live_key(const SnapshotLike& snapshot,render::ObjectKey key) {
    const auto* object=snapshot.instance(key.id);return object&&object->generation==key.generation;
}
template <class SnapshotLike>
std::optional<render::ObjectKey> articulation_owner(const SnapshotLike& snapshot,render::ObjectKey actor) {
    std::set<std::uint64_t> seen;
    auto* current=snapshot.instance(actor.id);
    for(unsigned depth=0;current&&depth<65;++depth) {
        if(current->class_name==QualifiedName{"physx3","PxArticulation"})return render::ObjectKey{current->id,current->generation};
        if(current->class_name!=QualifiedName{"physx3","PxArticulationLink"}||!seen.insert(current->id).second)return {};
        const auto* parent=current->property("Parent");
        if(!parent||!parent->reference_ids||parent->reference_ids->size()!=1)return {};
        current=snapshot.instance(parent->reference_ids->front());
    }
    return {};
}
} // namespace
bool visualization_scope_matches(VisualizationScope scope,bool selected) noexcept {
    switch(scope) {
    case VisualizationScope::none:return false;
    case VisualizationScope::all:return true;
    case VisualizationScope::selected:return selected;
    case VisualizationScope::unselected:return !selected;
    }
    return false;
}
std::set<render::PickingKey> visualization_selected_attachments(const FrameState& frame,const VisualizationSelection& selection,bool use_draw_selection) {
    std::set<render::PickingKey> result;
    const auto* object=selection.object?frame.snapshot.instance(selection.object->id):nullptr;
    if(selection.object&&(!object||object->generation!=selection.object->generation))return result;
    std::set<render::ObjectKey> aggregate_actors,aggregate_articulations;
    if(object&&object->class_name==QualifiedName{"physx3","PxAggregate"}) {
        for(const auto& entry:std::array<std::pair<const char*,std::set<render::ObjectKey>*>,2>{{{"Actors",&aggregate_actors},{"Articulations",&aggregate_articulations}}}) {
            const auto* value=object->property(entry.first);if(!value||!value->reference_ids)continue;
            if(value->reference_ids->size()>262144)throw std::length_error("visualization owner relationship budget exceeded");
            for(auto id:*value->reference_ids)if(const auto* member=frame.snapshot.instance(id))entry.second->insert({id,member->generation});
        }
    }
    std::map<render::ObjectKey,bool> owners;
    for(const auto& draw:frame.draws) {
        if(!selection.object&&!selection.attachment) {if(use_draw_selection&&draw.selected)result.insert(draw.key);continue;}
        if(!live_key(frame.snapshot,draw.key.scene)||!live_key(frame.snapshot,draw.key.actor)||!live_key(frame.snapshot,draw.key.shape))continue;
        bool selected=false;
        if(object) {
            const auto key=*selection.object;
            selected=draw.key.scene==key||draw.key.actor==key;
            if(object->class_name==QualifiedName{"physx3","PxArticulation"}||object->class_name==QualifiedName{"physx3","PxAggregate"}) {
                auto found=owners.find(draw.key.actor);
                if(found==owners.end()) {
                    bool member=false;
                    if(object->class_name.name=="PxArticulation")member=articulation_owner(frame.snapshot,draw.key.actor)==std::optional<render::ObjectKey>(key);
                    else {
                        member=aggregate_actors.count(draw.key.actor)!=0;
                        const auto owner=articulation_owner(frame.snapshot,draw.key.actor);
                        member|=owner&&aggregate_articulations.count(*owner)!=0;
                    }
                    found=owners.emplace(draw.key.actor,member).first;
                }
                selected|=found->second;
            } else if(draw.key.shape==key)
                selected|=!selection.attachment||draw.key==*selection.attachment;
            else if(selection.attachment&&draw.key==*selection.attachment)
                selected=true;
        } else if(selection.attachment)selected=draw.key==*selection.attachment;
        else selected=draw.selected;
        if(selected)result.insert(draw.key);
    }
    return result;
}
bool same_query_source(const render::SceneQuerySource &a,
                       const render::SceneQuerySource &b) noexcept {
  return a.scene == b.scene && a.kind == b.kind && a.batched == b.batched &&
      a.sequence == b.sequence && a.snapshot_event_count == b.snapshot_event_count &&
      a.publication_event_count == b.publication_event_count && a.stream_id == b.stream_id &&
      a.outer_timestamp == b.outer_timestamp && a.byte_offset == b.byte_offset;
}
DisplayAxes display_axes(const OrbitCamera &camera) {
  camera_valid(camera);
  const auto axes = camera_axes(camera);
  return {f(axes.x), f(axes.up), f(axes.z)};
}
void set_display_up(OrbitCamera &camera, DisplayUp up) {
  static constexpr Vec3 axes[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                  {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  const auto index = static_cast<std::size_t>(up);
  if (index >= std::size(axes))
    throw std::invalid_argument("unknown display up axis");
  auto next = camera;
  next.reference_up = axes[index];
  camera_valid(next);
  camera = next;
}
std::optional<DisplayUp> display_up(const OrbitCamera &camera) {
  const auto up = normal(d(camera.reference_up));
  static constexpr Vec3 axes[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                  {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  for (std::size_t i = 0; i < std::size(axes); ++i)
    if (dot(up, d(axes[i])) > 1 - 1e-6)
      return static_cast<DisplayUp>(i);
  return {};
}
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
bool valid_geometry_scale(float value) noexcept {
  return std::isfinite(value) && value >= .01f && value <= 10000.f;
}
Mat4 display_matrix(const DrawItem &item) {
  if (!valid_geometry_scale(item.geometry_scale))
    throw std::invalid_argument("geometry display scale must be in [.01,10000]");
  auto matrix = pose_matrix(item.world_pose);
  for (unsigned column = 0; column < 3; ++column)
    for (unsigned row = 0; row < 3; ++row)
      matrix.values[column * 4 + row] *= item.geometry_scale;
  return matrix;
}
Vec3 display_point(const DrawItem &item, Vec3 local) {
  if (!valid_geometry_scale(item.geometry_scale))
    throw std::invalid_argument("geometry display scale must be in [.01,10000]");
  const auto rotated = render::rotate(item.world_pose.rotation, local);
  return display_f(d(item.world_pose.position) + d(rotated) * item.geometry_scale);
}
std::optional<render::Bounds> display_bounds(const DrawItem &item) {
  if (!valid_geometry_scale(item.geometry_scale))
    throw std::invalid_argument("geometry display scale must be in [.01,10000]");
  if (item.display_world_bounds)
    return item.display_world_bounds;
  std::optional<render::Bounds> result;
  if (item.mesh)
    for (auto p : item.mesh->positions)
      include(result, display_point(item, p));
  return result;
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
  if (!std::isfinite(near_plane) || !std::isfinite(far_plane) ||
      near_plane <= 0 || far_plane <= near_plane)
    throw std::invalid_argument("invalid camera projection");
  auto b = basis(*this);
  auto t = std::tan(vertical_fov / 2);
  auto ray =
      normal(b.forward + b.right * ((2 * x / width - 1) * t * width / height) +
             b.up * ((1 - 2 * y / height) * t));
  Ray result{f(b.eye), f(ray)};
  result.view_clip = RayViewClip{f(b.forward), near_plane, far_plane};
  return result;
}
void OrbitCamera::orbit(double dx, double dy) {
  if (!std::isfinite(dx) || !std::isfinite(dy))
    return;
  yaw = std::remainder(
      yaw + dx * (handedness == DisplayHandedness::left_handed ? -1 : 1),
      2 * pi);
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
  const auto interval = ray_interval(input, direction, ray_clip_forward(input));
  if (!interval) return {};
  std::optional<PickHit> best;
  for (const auto &item : items) {
    if (!item.visible || !item.pickable || !item.mesh)
      continue;
    if (!valid_geometry_scale(item.geometry_scale) ||
        (item.face_culling != FaceCulling::none &&
         item.face_culling != FaceCulling::front &&
         item.face_culling != FaceCulling::back))
      throw std::invalid_argument("invalid shape display pick policy");
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
      auto a = d(mesh.positions[ia]) * item.geometry_scale,
           e1 = d(mesh.positions[ib]) * item.geometry_scale - a,
           e2 = d(mesh.positions[ic]) * item.geometry_scale - a;
      auto p = cross(dir, e2);
      auto determinant = dot(e1, p);
      auto scale = std::sqrt(dot(e1, e1) * dot(e2, e2));
      if (!std::isfinite(determinant) || !(scale > 0) ||
          std::abs(determinant) <=
              scale * std::numeric_limits<double>::epsilon() * 16)
        continue;
      if ((item.face_culling == FaceCulling::back && determinant < 0) ||
          (item.face_culling == FaceCulling::front && determinant > 0))
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
      if (!std::isfinite(t) || t < interval->first || t > interval->second ||
          (best && t >= best->distance))
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
  const auto clip_forward = ray_clip_forward(input);
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
    if (!finite(delta) || !std::isfinite(along) || along < input.min_distance ||
        along > input.max_distance ||
        (best && along >= best->distance))
      return;
    if (clip_forward) {
      const auto depth = dot(delta, *clip_forward);
      // Native markers are camera-facing quads at their center view-depth.
      // Their angular pick tolerance must not admit centers outside clipping.
      if (depth <= input.view_clip->near_depth || depth > input.view_clip->far_depth)
        return;
    }
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
      if (owner.pickable && visible(owner.key) &&
          cloth_color(owner, frame.palette)[3] > 0)
        for (std::size_t i = 0; i < owner.world_positions.size(); ++i)
          candidate(owner.key, deformables::ElementKind::cloth_vertex,
                    static_cast<std::uint32_t>(i), owner.world_positions[i]);
  if (options.particles)
    for (const auto &owner : frame.deformables.particle_clouds)
      if (owner.pickable && visible(owner.key) &&
          particle_color(owner, frame.palette)[3] > 0)
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
  struct WeakCacheEntry {
    std::weak_ptr<const render::TriangleBuffer> mesh;
    std::weak_ptr<const render::MeshData> resource;
    bool has_resource = false;
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
  render::SceneQueryIndex query_index;
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
  std::set<std::uint64_t> spatial_candidates;
  Playback playback;
  double remainder = 0;
  std::shared_ptr<FrameState> frame = std::make_shared<FrameState>();
  std::optional<PickingKey> selected;
  std::optional<deformables::SelectionKey> selected_deformable;
  std::optional<render::SceneQuerySource> selected_query;
  std::optional<ObjectKey> inspected;
  std::map<std::vector<std::uint64_t>, CacheEntry> cache;
  std::map<std::vector<std::uint64_t>, WeakCacheEntry> live_meshes;
  std::uint64_t clock = 0;
  struct ActorTreeCache {
    bool valid = false;
    std::size_t prefix = 0, metadata_prefix = 0;
    std::optional<std::uint64_t> scene;
    std::vector<ActorNode> nodes;
    std::map<QualifiedName, bool> class_roles;
  };
  mutable ActorTreeCache actor_tree_cache;
  explicit Impl(ModelOptions o) : options(o), extractor(o.extractor), query_index(o.scene_queries) {
    frame->label = "No recording";
  }
  // Derived display work can reject a valid source sample (for example scaled
  // coordinates beyond float range). Publish cursor/selection together with
  // its immutable FrameState. Accepted live Session data is never rolled back.
  struct CursorGuard {
    Impl &owner;
    std::optional<std::uint64_t> scene;
    SamplePoint sample;
    std::size_t index;
    Playback playback;
    double remainder;
    bool follow, with_timeline, committed = false;
    std::optional<PickingKey> selected;
    std::optional<deformables::SelectionKey> selected_deformable;
    std::optional<render::SceneQuerySource> selected_query;
    std::optional<ObjectKey> inspected;
    std::shared_ptr<FrameState> frame;
    std::vector<TimelineEntry> timeline;
    explicit CursorGuard(Impl &value, bool save_timeline = false)
        : owner(value), scene(value.scene), sample(value.sample), index(value.index),
          playback(value.playback), remainder(value.remainder), follow(value.follow),
          with_timeline(save_timeline), selected(value.selected),
          selected_deformable(value.selected_deformable), selected_query(value.selected_query),
          inspected(value.inspected), frame(value.frame) {
      if (with_timeline) timeline.swap(value.timeline);
    }
    ~CursorGuard() noexcept { if (!committed) rollback(); }
    void rollback() noexcept {
      owner.scene = scene; owner.sample = sample; owner.index = index;
      owner.playback = playback; owner.remainder = remainder; owner.follow = follow;
      owner.selected = selected; owner.selected_deformable = selected_deformable;
      owner.selected_query = selected_query; owner.inspected = inspected;
      owner.frame = std::move(frame);
      if (with_timeline) owner.timeline.swap(timeline);
      committed = true;
    }
    void commit() noexcept { committed = true; }
  };
  void pause_live_display(const char *reason) {
    auto next = std::make_shared<FrameState>(*frame);
    const std::string prefix = "Live display paused: ";
    next->errors.erase(std::remove_if(next->errors.begin(), next->errors.end(),
        [&](const auto &error) { return error.rfind(prefix, 0) == 0; }), next->errors.end());
    next->errors.push_back(prefix + reason + ". Source events through " +
        std::to_string(session->event_count()) + " remain accepted; lower geometry "
        "display scale and enable Follow to show the latest sample.");
    frame = std::move(next);
    follow = false;
    playback.playing = false;
    remainder = 0;
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
    std::shared_ptr<const render::MeshData> resource;
    std::visit([&](const auto &value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, render::ConvexMesh> ||
                    std::is_same_v<T, render::TriangleMesh> ||
                    std::is_same_v<T, render::HeightField>)
        resource = value.mesh;
    }, g);
    auto live = live_meshes.find(key);
    std::shared_ptr<const render::TriangleBuffer> data;
    if (live != live_meshes.end() &&
        (!live->second.has_resource || live->second.resource.lock() == resource))
      data = live->second.mesh.lock();
    if (!data)
      data = std::make_shared<const render::TriangleBuffer>(
          render::tessellate(g, options.mesh));
    if (options.max_tessellation_cache_entries) {
      live_meshes[key] = {data, resource, bool(resource)};
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
      if (!(life.class_name == QualifiedName{"physx3", "PxMaterial"}))
        spatial_candidates.insert(life.id);
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
        if (auto object = session->instance_at_event(f.event_count, f.scene_id))
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

  bool shape_visible(const render::ShapeInstance &s, bool selection_member) const {
    if (!view.show_planes &&
        render::geometry_kind(s.geometry) == render::GeometryKind::plane)
      return false;
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
    if (s.actor.sleeping == true &&
        (!view.show_sleeping ||
         !visualization_scope_matches(view.sleeping_scope, selection_member)))
      return false;
    return !s.flags.trigger ||
           (view.show_trigger &&
            visualization_scope_matches(view.trigger_scope, selection_member));
  }
  void build_joint_display(FrameState &state) const {
    auto *next = &state;
    next->errors.erase(std::remove_if(next->errors.begin(), next->errors.end(),
        [](const auto &error) { return error.rfind("Joint display channel ", 0) == 0; }),
        next->errors.end());
    std::vector<render::DebugConstraint> frames, limits;
    for (const auto &constraint : next->debug.constraints)
      (std::holds_alternative<render::DebugJointFrames>(constraint.payload)
           ? frames : limits).push_back(constraint);
    using DebugLocation = std::tuple<std::size_t, std::size_t, std::uint64_t>;
    std::set<DebugLocation> handled;
    bool complete = true;
    auto request = options.constraints;
    request.frame_scale *= view.joint_frame_scale;
    request.limit_scale *= view.joint_limit_scale;
    const auto build = [&](const auto &input, auto &output) {
      auto built = render::build_constraint_lines(input, request);
      complete &= built.complete;
      std::set<std::size_t> rejected;
      for (const auto &diagnostic : built.diagnostics) {
        rejected.insert(diagnostic.constraint_index);
        next->errors.push_back("Joint display channel " +
            std::to_string(diagnostic.source.channel_instance_id) + " event " +
            std::to_string(diagnostic.source.event_index) + " offset " +
            std::to_string(diagnostic.source.payload_offset) + ": " + diagnostic.message);
      }
      for (std::size_t i = 0; i < built.constraints_processed; ++i)
        if (!rejected.count(i)) {
          const auto &source = input[i].source;
          handled.emplace(source.event_index, source.payload_offset,
                          source.channel_instance_id);
        }
      request.max_lines -= built.lines.size();
      request.max_constraints -= std::min(request.max_constraints,
                                          built.constraints_processed);
      output = std::move(built.lines);
    };
    build(frames, next->joint_frame_lines);
    build(limits, next->joint_limit_lines);
    next->debug.diagnostics.erase(std::remove_if(next->debug.diagnostics.begin(),
        next->debug.diagnostics.end(), [&](const auto &diagnostic) {
          return diagnostic.code == render::DebugCode::constraint_not_tessellated &&
              handled.count({diagnostic.event_index, diagnostic.payload_offset,
                              diagnostic.channel_instance_id});
        }), next->debug.diagnostics.end());
    next->debug.complete = next->debug.diagnostics.empty() && complete;
    next->joint_frame_scale = view.joint_frame_scale;
    next->joint_limit_scale = view.joint_limit_scale;
  }
  void apply_view(FrameState &next) const {
    next.palette = view.palette;
    if (next.joint_frame_scale != view.joint_frame_scale ||
        next.joint_limit_scale != view.joint_limit_scale)
      build_joint_display(next);
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
    for (const auto *lines : {&next.joint_frame_lines, &next.joint_limit_lines})
      for (const auto &line : *lines) {
        include(next.bounds, line.position0);
        include(next.bounds, line.position1);
      }
    std::map<PickingKey, const render::ShapeInstance *> shapes;
    for (const auto &shape : next.render.instances)
      shapes.emplace(shape.picking_key(), &shape);
    const auto selection = visualization_selected_attachments(
        next, {inspected, selected}, false);
    for (auto &draw : next.draws) {
      const auto found = shapes.find(draw.key);
      draw.visible = found != shapes.end() &&
                     shape_visible(*found->second, selection.count(draw.key) != 0);
      if (found != shapes.end())
        draw.color = display_color(*found->second, view);
      draw.geometry_scale = view.geometry_scale;
      draw.display_world_bounds.reset();
      if (found != shapes.end() && found->second->world_bounds) {
        const auto pivot = d(draw.world_pose.position);
        const auto &raw = *found->second->world_bounds;
        draw.display_world_bounds = view.geometry_scale == 1 ? raw : render::Bounds{
            display_f(pivot + (d(raw.minimum) - pivot) * view.geometry_scale),
            display_f(pivot + (d(raw.maximum) - pivot) * view.geometry_scale)};
      }
      if (draw.visible) {
        if (auto bounds = draw_bounds(draw))
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
    if (selected_query && std::none_of(frame->scene_queries.queries.begin(),
        frame->scene_queries.queries.end(), [&](const auto &query) {
          return same_query_source(query.source, *selected_query);
        })) selected_query.reset();
    if (selected_deformable &&
        !deformable_key_valid(*frame, *selected_deformable))
      selected_deformable.reset();
    if (selected) {
      auto found = std::find_if(
          frame->render.instances.begin(), frame->render.instances.end(),
          [&](const auto &s) { return s.picking_key() == *selected; });
      if (found == frame->render.instances.end()) {
        // A shared PxShape may survive the selected actor attachment. Do not
        // reinterpret that retired attachment as an explicit all-owner shape
        // selection on the next refresh or reverse seek.
        if (inspected == std::optional<ObjectKey>(selected->shape))
          inspected.reset();
        selected.reset();
      }
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
    frame->scene_query_selection = selected_query;
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
    for (auto it = live_meshes.begin(); it != live_meshes.end();) {
      if (it->second.mesh.expired() ||
          (it->second.has_resource && it->second.resource.expired()))
        it = live_meshes.erase(it);
      else
        ++it;
    }
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
      next->snapshot = session->snapshot_view_at_event(timeline[index].event_count);
    } else {
      next->label = live_ingestion ? "Live capture: waiting for a completed "
                                     "frame (safe current state)"
                    : sample == SamplePoint::after_fetch
                        ? "No explicit after-fetch markers; showing capture EOF"
                        : "No completed frames; showing capture EOF";
      next->snapshot = session->snapshot_view_at_event(session->event_count());
    }
    std::vector<std::uint64_t> spatial_ids;
    spatial_ids.reserve(std::min(spatial_candidates.size(), next->snapshot.instances.size()));
    for (const auto id : spatial_candidates)
      if (const auto *object = next->snapshot.instance(id);
          object && !(object->class_name == QualifiedName{"physx3", "PxMaterial"}))
        spatial_ids.push_back(id);
    next->spatial_snapshot = std::make_shared<const SnapshotView>(
        next->snapshot.select_view(spatial_ids));
    render::ExtractOptions selection;
    selection.scene_id = scene;
    if (selected)
      selection.selected.push_back(*selected);
    next->render =
        extractor.extract(session->metadata(), *next->spatial_snapshot, selection);
    auto deformable_options = options.deformables;
    // A scene may not yet exist at this historical sample. Match the live
    // generation from the snapshot, never a cached later lifetime.
    if (scene) {
      const auto *object = next->snapshot.instance(*scene);
      if (object) {
        deformable_options.scene =
            deformables::ObjectKey{object->id, object->generation};
        next->deformables = deformables::extract(
            session->metadata(), *next->spatial_snapshot, deformable_options);
      } else
        next->deformables.event_count = next->snapshot.event_count;
    } else
      next->deformables = deformables::extract(
          session->metadata(), *next->spatial_snapshot, deformable_options);
    render::DebugRange range;
    if (next->marker) {
      range.begin_event_count = next->marker->begin_event_count;
      range.end_event_count = next->marker->event_count;
    } else
      range.end_event_count = next->snapshot.event_count;
    range.scene_id = scene;
    if (scene && sample == SamplePoint::native_frame) {
      next->scene_queries = query_index.extract(*session,
          render::SceneQueryWindow{*scene, range.begin_event_count, range.end_event_count});
      std::size_t volume_triangles = 0;
      std::map<std::size_t, Snapshot> query_snapshots;
      bool volume_limit = false;
      const auto volume_error = [&](const std::string &message) {
        if (next->errors.size() < 256) next->errors.push_back("Scene query volume: " + message);
      };
      for (const auto &query : next->scene_queries.queries) {
        if (query.coordinates_ambiguous) continue;
        if (query.geometries.empty()) continue;
        if (volume_limit) break;
        auto &source_snapshot = query_snapshots[query.source.snapshot_event_count];
        source_snapshot.event_count = query.source.snapshot_event_count;
        for (std::size_t i = 0; i < query.geometries.size(); ++i) {
          std::optional<Transform> pose;
          if (i < query.poses.size()) pose = query.poses[i];
          else if (!i && query.overlap_pose) pose = query.overlap_pose;
          if (!pose) { volume_error("recorded geometry has no matching recorded pose"); continue; }
          const auto geometry_id = query.geometries[i].key.id;
          if (!source_snapshot.instance(geometry_id)) {
            auto object = session->instance_at_event(source_snapshot.event_count, geometry_id);
            if (object) source_snapshot.instances.emplace(geometry_id, std::move(*object));
          }
          if (const auto *object = source_snapshot.instance(geometry_id))
            for (const auto *property : {"ConvexMesh", "TriangleMesh", "HeightField"})
              if (const auto *value = object->property(property))
                if (auto id = value->as_object_ref(); id && *id && !source_snapshot.instance(*id)) {
                  auto resource = session->instance_at_event(source_snapshot.event_count, *id);
                  if (resource) source_snapshot.instances.emplace(*id, std::move(*resource));
                }
          auto geometry = extractor.extract_geometry(session->metadata(), source_snapshot, query.geometries[i].key);
          if (!geometry.geometry) {
            for (const auto &diagnostic : geometry.diagnostics)
              volume_error("geometry " + std::to_string(query.geometries[i].key.id) + ": " + diagnostic.message);
            continue;
          }
          try {
            auto mesh = triangles(*geometry.geometry);
            const auto count = mesh->indices.size() / 3;
            if (count > options.max_query_volume_triangles - volume_triangles) {
              volume_error("preview triangle budget reached; remaining recorded volumes are not drawn");
              volume_limit = true;
              break;
            }
            for (const auto &point : mesh->positions) {
              auto world = render::transform_point(*pose, point);
              if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
                throw std::overflow_error("query volume world coordinates overflow");
            }
            volume_triangles += count;
            next->query_volumes.push_back({query.source, std::move(mesh), *pose});
          } catch (const std::exception &e) { volume_error(e.what()); }
        }
      }
    } else {
      next->scene_queries.event_count = next->snapshot.event_count;
      for (std::size_t i = 0; i < 6; ++i) {
        next->scene_queries.collections[i].kind = static_cast<render::SceneQueryKind>(i % 3);
        next->scene_queries.collections[i].batched = i >= 3;
      }
    }
    auto debug_options = options.debug;
    debug_options.joint_axis_length = 0;
    next->debug =
        render::extract_debug(*next->spatial_snapshot, session->metadata(),
                              session->channels(), range, debug_options);
    build_joint_display(*next);
    for (const auto &s : next->render.instances) {
      try {
        DrawItem item{triangles(s.geometry),
                      s.world_pose,
                      s.picking_key(),
                      display_color(s, view),
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
  SessionOptions session_options;
  session_options.max_checkpoints = 0;
  adopt(std::make_unique<Session>(*decoder.header(), decoder.take_events(), session_options),
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
  next->view = impl_->view;
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
  Impl::CursorGuard cursor(*impl_);
  try { if (impl_->follow) {
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
  } } catch (const DisplayOverflow &error) {
    cursor.rollback();
    impl_->pause_live_display(error.what());
    return true;
  }
  // Paused historical FrameState is immutable and is not rebuilt on polling.
  // timeline()/scenes() expose current counts independently of that
  // publication.
  cursor.commit();
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
  Impl::CursorGuard cursor(*impl_);
  try {
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
  } catch (const DisplayOverflow &error) {
    cursor.rollback();
    impl_->pause_live_display(error.what());
    return;
  }
  cursor.commit();
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
  Impl::CursorGuard cursor(*impl_, true);
  impl_->scene = id;
  if (impl_->sample == SamplePoint::after_fetch &&
      impl_->after_timelines[id].empty())
    impl_->sample = SamplePoint::native_frame;
  impl_->selected.reset();
  impl_->selected_deformable.reset();
  impl_->inspected.reset();
  impl_->make_timeline();
  impl_->build_frame();
  cursor.commit();
  return true;
}
SamplePoint Document::sample_point() const noexcept { return impl_->sample; }
bool Document::set_sample_point(SamplePoint s) {
  if (s != SamplePoint::native_frame && s != SamplePoint::after_fetch)
    throw std::invalid_argument("unknown sample point");
  if (!loaded())
    return false;
  if (s == SamplePoint::after_fetch &&
      (!impl_->scene || impl_->after_timelines[*impl_->scene].empty()))
    return false;
  Impl::CursorGuard cursor(*impl_, true);
  impl_->sample = s;
  impl_->make_timeline();
  impl_->build_frame();
  cursor.commit();
  return true;
}
const std::vector<TimelineEntry> &Document::timeline() const noexcept {
  return impl_->timeline;
}
std::size_t Document::frame_index() const noexcept { return impl_->index; }
bool Document::set_frame(std::size_t i) {
  if (i >= impl_->timeline.size())
    return false;
  Impl::CursorGuard cursor(*impl_);
  impl_->playback.playing = false;
  impl_->follow = false;
  impl_->remainder = 0;
  if (i == impl_->index) {
    cursor.commit();
    return true;
  }
  impl_->index = i;
  impl_->build_frame();
  cursor.commit();
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
  Impl::CursorGuard cursor(*impl_);
  auto frames =
      impl_->remainder + elapsed * std::abs(impl_->playback.frames_per_second);
  if (!std::isfinite(frames))
    throw std::overflow_error("elapsed playback time overflow");
  auto whole = std::floor(frames);
  impl_->remainder = frames - whole;
  if (whole < 1) {
    cursor.commit();
    return false;
  }
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
  if (impl_->index == old) {
    cursor.commit();
    return false;
  }
  impl_->build_frame();
  cursor.commit();
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
std::optional<render::SceneQuerySource> Document::selected_query() const noexcept {
  return impl_->selected_query;
}
bool Document::select_query(const render::SceneQuerySource &requested) {
  const auto &queries = impl_->frame->scene_queries.queries;
  if (std::none_of(queries.begin(), queries.end(), [&](const auto &query) {
        return same_query_source(query.source, requested);
      })) return false;
  impl_->selected_query = requested;
  impl_->selected.reset();
  impl_->selected_deformable.reset();
  impl_->inspected.reset();
  impl_->refresh_view();
  return true;
}
bool Document::fit_query(OrbitCamera &camera, double aspect) const {
  if (!impl_->selected_query) return false;
  const auto &source = *impl_->selected_query;
  for (const auto &query : impl_->frame->scene_queries.queries)
    if (same_query_source(query.source, source) && query.coordinates_ambiguous)
      return false;
  std::optional<render::Bounds> bound;
  auto point = [&](Vec3 p) {
    include(bound, render::Bounds{p, p});
  };
  for (const auto &line : impl_->frame->scene_queries.lines)
    if (same_query_source(line.source, source)) { point(line.position0); point(line.position1); }
  for (const auto &volume : impl_->frame->query_volumes)
    if (volume.mesh && same_query_source(volume.source, source))
      for (const auto &p : volume.mesh->positions) point(render::transform_point(volume.pose, p));
  for (const auto &query : impl_->frame->scene_queries.queries)
    if (same_query_source(query.source, source))
      for (const auto &hit : query.hits) if (hit.position_available) point(hit.position);
  if (!bound) return false;
  camera.fit(*bound, aspect);
  return true;
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
  impl_->selected_query.reset();
  impl_->selected_deformable = key;
  impl_->inspected =
      ObjectKey{key.object.owner.id, key.object.owner.generation};
  impl_->refresh_view();
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
  impl_->selected_query.reset();
  impl_->selected_deformable.reset();
  impl_->inspected = key.shape;
  impl_->refresh_view();
  return true;
}
bool Document::inspect(std::uint64_t id) {
  auto object = impl_->frame->snapshot.instance(id);
  if (!object)
    return false;
  impl_->selected_query.reset();
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
  impl_->refresh_view();
  return true;
}

void Document::clear_selection() {
  impl_->selected.reset();
  impl_->selected_query.reset();
  impl_->selected_deformable.reset();
  impl_->inspected.reset();
  impl_->refresh_view();
}
std::vector<ActorNode> Document::actor_tree() const {
  std::vector<ActorNode> result;
  if (!loaded())
    return result;
  auto &cache = impl_->actor_tree_cache;
  const auto prefix = impl_->frame->snapshot.event_count;
  // Metadata is global to Session. An accepted future DeriveClass can change
  // classification even while the displayed snapshot is paused, so use the
  // accepted event count as a conservative metadata invalidation token.
  const auto metadata_prefix = impl_->session->event_count();
  if (cache.valid && cache.prefix == prefix &&
      cache.metadata_prefix == metadata_prefix && cache.scene == impl_->scene)
    return cache.nodes;
  if (cache.metadata_prefix != metadata_prefix) cache.class_roles.clear();
  const auto &snapshot = impl_->frame->snapshot;
  const auto &m = impl_->session->metadata();
  std::vector<const InstanceState *> candidates;
  const auto gather = [&](const auto &objects) {
    candidates.reserve(objects.size());
    for (const auto &[id, object] : objects) candidates.push_back(&object);
  };
  if (impl_->frame->spatial_snapshot)
    gather(impl_->frame->spatial_snapshot->instances);
  else
    gather(snapshot.instances);
  for (const auto *candidate : candidates) {
    const auto &object = *candidate;
    const auto id = object.id;
    auto role = cache.class_roles.find(object.class_name);
    if (role == cache.class_roles.end())
      role = cache.class_roles.emplace(object.class_name,
          is_a(m, object, "PxRigidActor") || is_a(m, object, "PxRigidStatic") ||
          is_a(m, object, "PxRigidDynamic") || is_a(m, object, "PxArticulationLink")).first;
    if (!role->second)
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
  cache.nodes.swap(result);
  cache.prefix = prefix;
  cache.metadata_prefix = metadata_prefix;
  cache.scene = impl_->scene;
  cache.valid = true;
  return cache.nodes;
}
bool Document::fit_scene(OrbitCamera &c, double aspect) const {
  if (!impl_->frame->bounds)
    return false;
  c.fit(*impl_->frame->bounds, aspect);
  return true;
}
bool Document::fit_selected(OrbitCamera &c, double aspect) const {
  if (impl_->selected_query) return fit_query(c, aspect);
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
  case ColorMode::recorded_status: {
    std::vector<ColorLegendEntry> result;
    for (const auto &entry : status_palette_legend(v.palette))
      result.push_back({entry.label, entry.color});
    return result;
  }
  case ColorMode::actor_kind: {
    std::vector<ColorLegendEntry> result;
    for (const auto &entry : actor_kind_palette_legend(v.palette))
      result.push_back({entry.label, entry.color});
    return result;
  }
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
  if (!valid_color_palette(options.palette))
    throw std::invalid_argument("palette RGBA components must be finite in [0,1]");
  if (static_cast<int>(options.color_mode) < 0 ||
      static_cast<int>(options.color_mode) >
          static_cast<int>(ColorMode::uniform))
    throw std::invalid_argument("unknown viewer color mode");
  for (const auto scope : {options.sleeping_scope, options.trigger_scope})
    if (scope != VisualizationScope::none && scope != VisualizationScope::all &&
        scope != VisualizationScope::selected &&
        scope != VisualizationScope::unselected)
      throw std::invalid_argument("unknown viewer status scope");
  if (!valid_geometry_scale(options.geometry_scale))
    throw std::invalid_argument("geometry display scale must be in [.01,10000]");
  for (const auto scale : {options.joint_frame_scale, options.joint_limit_scale})
    if (!std::isfinite(scale) || scale < 0 || scale > 10000)
      throw std::invalid_argument("joint display scale must be in [0,10000]");
  if (same_view_options(impl_->view, options))
    return;
  auto previous = impl_->view;
  impl_->view = options;
  try {
    impl_->refresh_view();
  } catch (...) {
    impl_->view = previous;
    throw;
  }
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
          if (auto pose = coordinates::native_transform(*value);
              pose && finite(d(pose->position)))
            return pose->position;
        } catch (const std::exception &) {
        }
  }
  return {};
}
} // namespace
void stopTracking(SceneToolState &state) noexcept {
  state.recorded_follow.reset();
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
  if (state.recorded_follow.enabled)
    return updateRecordedCameraFollow(doc, camera, state);
  if (!state.tracking_object())
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
  const auto frame = doc.frame();
  const auto found = frame->snapshot.cameras.find(name);
  if (found == frame->snapshot.cameras.end()) {
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
