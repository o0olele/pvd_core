#pragma once
#include "pvd/debug_render.hpp"
#include "pvd/deformables.hpp"
#include "pvd/mesh_builder.hpp"
#include <array>
#include <memory>

namespace pvd::viewer {
using render::ObjectKey;
using render::PickingKey;
// Column-major m[column*4+row], column vectors, right-handed world, Y up.
// Projection has D3D depth 0..1. HLSL: column_major matrix; mul(MVP,position).
struct Mat4 {
  std::array<float, 16> values{};
};
Mat4 multiply(const Mat4 &, const Mat4 &);
Mat4 pose_matrix(const Transform &);
struct Ray {
  Vec3 origin, direction;
}; // Direction normalized by camera helpers.
struct DrawItem {
  std::shared_ptr<const render::TriangleBuffer> mesh;
  Transform world_pose;
  PickingKey key;
  Color color;
  bool pickable = true, selected = false;
  bool visible =
      true; // Viewer state only; hidden draws are never CPU-pickable.
};
struct PickHit {
  PickingKey key;
  double distance = 0;
  Vec3 position;
  std::size_t triangle_index = 0;
};
std::optional<PickHit> pick(const std::vector<DrawItem> &, const Ray &);
class OrbitCamera {
public:
  Vec3 target{0, 0, 0};
  Vec3 reference_up{0, 1, 0}; // Recorded arbitrary up; default SDK Y-up.
  double yaw = 0.75, pitch = 0.45, distance = 10;
  double vertical_fov = 0.7853981633974483, near_plane = 0.01,
         far_plane = 1000000;
  Vec3 position() const;
  Mat4 view_matrix() const;
  Mat4 projection_matrix(double aspect) const;
  Mat4 view_projection(double aspect) const;
  Ray screen_ray(double x, double y, double width, double height) const;
  void orbit(double delta_yaw, double delta_pitch);
  void pan(double delta_x_pixels, double delta_y_pixels,
           double viewport_height);
  void zoom(double wheel_steps);
  void fit(const render::Bounds &, double aspect = 1.0);
  // Exact look-at basis. Rejects non-finite/coincident/collinear input.
  void set_look_at(const CameraState &);
};
enum class ColorMode {
  recorded_status,
  actor_kind,
  geometry,
  actor_identity,
  uniform
};
struct ViewOptions {
  // Static / non-kinematic Dynamic / Kinematic are independent categories.
  // Sleeping and Trigger are additional filters. Unknown sleep state is shown.
  bool show_static = true, show_dynamic = true, show_kinematic = true,
       show_sleeping = true, show_trigger = true;
  ColorMode color_mode = ColorMode::recorded_status;
  Color uniform_color{160, 180, 205, 255};
};
struct ColorLegendEntry {
  std::string label;
  Color color;
};
std::vector<ColorLegendEntry> view_legend(const ViewOptions &);
struct ObjectNode {
  ObjectKey key;
  std::string name, class_name;
};
struct ObjectGroup {
  QualifiedName class_name;
  std::vector<ObjectNode> objects;
};
enum class SamplePoint { native_frame, after_fetch };
struct SceneInfo {
  ObjectKey key;
  std::string name;
  std::size_t native_frames = 0, after_fetch_frames = 0;
};
struct TimelineEntry {
  std::uint64_t scene_id = 0, stream_id = 0;
  std::size_t index = 0, begin_event_count = 0, event_count = 0;
  SamplePoint sample_point = SamplePoint::native_frame;
  std::string label;
};
struct ShapeNode {
  ObjectKey key;
  std::string name, class_name;
  bool present = true;
  std::optional<std::uint64_t> geometry_id;
};
struct ActorNode {
  ObjectKey key;
  std::string name, class_name;
  std::vector<ShapeNode> shapes;
};
struct FrameState {
  Snapshot snapshot;
  render::Frame render;
  render::DebugResult debug;
  deformables::Frame deformables;
  struct DeformableDraw {
    deformables::OwnerKey key;
    bool visible = true, selected = false;
  };
  std::vector<DeformableDraw> deformable_draws;
  std::optional<deformables::SelectionKey> deformable_selection;
  std::vector<DrawItem> draws;
  std::optional<render::Bounds> bounds;
  std::optional<TimelineEntry> marker;
  std::size_t frame_index = 0, frame_count = 0;
  std::string label;
  std::vector<std::string> errors;
};
struct ModelOptions {
  render::ExtractorOptions extractor;
  render::MeshBuildOptions mesh;
  render::DebugOptions debug;
  deformables::Options deformables;
  std::size_t max_tessellation_cache_entries = 128;
};
struct Playback {
  bool playing = false, loop = false;
  double frames_per_second = 60;
};
class Document {
public:
  explicit Document(ModelOptions options = {});
  ~Document();
  Document(Document &&) noexcept;
  Document &operator=(Document &&) noexcept;
  Document(const Document &) = delete;
  Document &operator=(const Document &) = delete;
  // Synchronous strong replacement. Host may load a separate Document on a
  // worker and transfer ownership to the UI; never mutate one Document
  // concurrently.
  void open_file(const std::string &);
  void adopt(std::unique_ptr<Session>, std::string source_label = {});
  // Incremental live ingestion. Caller serializes these with all UI queries.
  // Partial property transactions stay pending; the published frame is retained
  // until a complete boundary. No accepted event prefix is reparsed/rebuilt.
  void begin_live(StreamHeader, std::string source_label = {},
                  SessionOptions options = {});
  bool append_live(std::vector<Event>);
  void finish_live();
  bool live() const noexcept;
  std::size_t live_event_count() const noexcept;
  bool follow_live() const noexcept;
  void set_follow_live(bool);
  bool loaded() const noexcept;
  const Session *session() const noexcept;
  const Metadata *metadata() const;
  const std::string &source_label() const noexcept;
  const std::vector<SceneInfo> &scenes() const noexcept;
  std::optional<std::uint64_t> scene_id() const noexcept;
  bool set_scene(std::uint64_t);
  SamplePoint sample_point() const noexcept;
  bool set_sample_point(SamplePoint);
  const std::vector<TimelineEntry> &timeline() const noexcept;
  std::size_t frame_index() const noexcept;
  bool set_frame(std::size_t);
  bool step(std::ptrdiff_t delta);
  const Playback &playback() const noexcept;
  void set_playback(Playback);
  void set_playing(bool);
  // Tick exactly once per host frame. Returns true if the capture frame
  // changed.
  bool advance(double elapsed_seconds);
  std::shared_ptr<const FrameState> frame() const noexcept;
  std::optional<PickingKey> selected_key() const noexcept;
  std::optional<deformables::SelectionKey> selected_deformable() const noexcept;
  // Owner selection survives matching live generations. Element selection is
  // scoped to the exact current sample, so slot reuse/seeking cannot retarget
  // it.
  bool select_deformable(const deformables::SelectionKey &);
  bool deformable_visible(const deformables::OwnerKey &) const noexcept;
  void set_deformable_visible(const deformables::OwnerKey &, bool);
  void isolate_deformable(const deformables::OwnerKey &);
  bool fit_deformable(const deformables::OwnerKey &, OrbitCamera &,
                      double aspect = 1.0) const;
  std::optional<ObjectKey> inspected_object() const noexcept;
  const InstanceState *inspected_instance() const noexcept;
  bool select(
      const PickingKey &); // Viewport shape selection also inspects that shape.
  bool
  inspect(std::uint64_t object_id); // Arbitrary object/reference navigation.
  void clear_selection();
  std::vector<ActorNode> actor_tree() const;
  // All captured live objects at this snapshot, including shared/global
  // resources and objects belonging to other scenes. Grouping is
  // metadata-based.
  std::vector<ObjectGroup> object_groups() const;
  const ViewOptions &view_options() const noexcept;
  void set_view_options(ViewOptions);
  bool visible(const PickingKey &) const noexcept;
  void set_visible(const PickingKey &, bool);
  void set_actor_visible(ObjectKey scene, ObjectKey actor, bool);
  void isolate(const PickingKey &);
  void isolate_actor(ObjectKey scene, ObjectKey actor);
  bool isolated() const noexcept;
  // Clears hide/isolate rules and enables every category. Keeps color mode.
  void show_all();
  bool fit_scene(OrbitCamera &, double aspect = 1.0) const;
  bool fit_selected(OrbitCamera &, double aspect = 1.0) const;
  std::size_t tessellation_cache_entries() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct DeformablePickOptions {
  bool cloth = true, particles = true;
  // A world-space floor plus a perspective screen-size tolerance. For N
  // pixels, use angular_radius=N*2*tan(fov/2)/viewport_height.
  double world_radius = 0.025, angular_radius = 0;
};
struct DeformablePickHit {
  deformables::SelectionKey key;
  double distance = 0;
  Vec3 position;
};
std::optional<DeformablePickHit>
pick_deformable(const FrameState &, const Ray &,
                const DeformablePickOptions & = {});
} // namespace pvd::viewer
