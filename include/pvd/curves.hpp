#pragma once
#include "pvd/analysis_export.hpp"
#include "pvd/session.hpp"
#include <functional>
#include <iosfwd>
#include <limits>

namespace pvd::analysis {
struct ObjectKey {
  std::uint64_t id = 0, generation = 0;
  bool operator==(const ObjectKey &o) const {
    return id == o.id && generation == o.generation;
  }
};
enum class Sampling { native_frame, after_fetch };
enum class CoordinateSpace { native, wire };
struct FrameSource {
  std::uint64_t scene_id = 0;
  std::optional<std::uint64_t> stream_id;
  Sampling sampling = Sampling::native_frame;
  // Explicit SDK/application marker only; never infer post-simulation from
  // native EndSection.
  std::string after_fetch_section = "validation_after_fetch";
};
struct FramePoint {
  FrameSource source;
  std::size_t frame_index = 0;
};
struct SampleLocation {
  // Ordinal in this scene/mode timeline BEFORE optional stream filtering. This
  // aligns with the viewer's per-scene timeline, including multi-stream scenes.
  std::size_t frame_index = 0, event_count = 0;
  std::uint64_t scene_id = 0, stream_id = 0, timestamp_ticks = 0;
};
PVD_ANALYSIS_API std::optional<SampleLocation> locate_frame(const Session &,
                                                            const FramePoint &);
using Numeric = std::variant<std::uint64_t, std::int64_t, double>;
PVD_ANALYSIS_API double plot_value(const Numeric &) noexcept;
PVD_ANALYSIS_API std::string numeric_text(const Numeric &);
PVD_ANALYSIS_API std::optional<double>
timestamp_seconds(const StreamHeader &, std::uint64_t ticks) noexcept;
enum class Gap {
  none,
  object_absent,
  generation_mismatch,
  property_missing,
  item_missing,
  not_numeric,
  unsupported_type,
  invalid_component,
  malformed_value,
  non_finite
};
PVD_ANALYSIS_API const char *gap_name(Gap) noexcept;
struct CurveRequest {
  FrameSource source;
  ObjectKey object;
  std::string property, component;
  std::size_t item_index = 0;
  std::size_t first_frame = 0,
              end_frame = std::numeric_limits<std::size_t>::max(), stride = 1;
  CoordinateSpace coordinate_space = CoordinateSpace::native;
};
enum class Resampling {
  reject,
  uniform
}; // Uniform selects original rows, including gaps; no interpolation.
struct CurveOptions {
  std::size_t max_samples = 10000;
  Resampling resampling = Resampling::uniform;
  // Return true to stop before the next snapshot. Returned rows remain valid;
  // cancelled/complete explicitly describe the partial result.
  std::function<bool()> cancelled;
  std::function<void(std::size_t completed, std::size_t total)> progress;
};
struct CurveSample {
  SampleLocation source;
  std::optional<double> seconds, elapsed_seconds;
  std::optional<Numeric> value;
  Gap gap = Gap::none;
  // Unsampled timeline rows lie before this point: never draw a continuous
  // segment from the preceding point across an unobserved interval.
  bool break_before = false;
};
struct Series {
  CurveRequest request;
  std::size_t source_event_count = 0, candidate_samples = 0;
  std::vector<CurveSample> samples;
  std::vector<std::string> diagnostics;
  bool complete = true, cancelled = false, resampled = false;
};
PVD_ANALYSIS_API Series sample_curve(const Session &, CurveRequest,
                                     CurveOptions options = {});
// RFC4180; integer values and raw ticks are decimal integer text, never through
// double. Floating values use max_digits10 and the classic locale. Gaps are
// empty.
PVD_ANALYSIS_API void write_curve_csv(std::ostream &, const Series &);

struct InstanceAttributes {
  QualifiedName class_name;
  bool pickable = true, top_level = false;
  std::optional<Color> color;
};
struct PropertyChange {
  std::string name;
  std::optional<PropertyValue> before, after;
  bool type_changed = false, data_changed = false, references_changed = false,
       text_changed = false;
  bool wire_data_changed = false, coordinates_changed = false;
};
enum class ChangeKind { created, destroyed, changed };
struct ObjectChange {
  ObjectKey object;
  ChangeKind kind = ChangeKind::changed;
  std::optional<InstanceAttributes> before, after;
  bool class_changed = false, pickable_changed = false,
       top_level_changed = false, color_changed = false;
  std::vector<PropertyChange> properties;
};
struct CameraChange {
  std::string name;
  std::optional<CameraState> before, after;
};
struct OriginShiftChange {
  std::uint64_t scene_id = 0;
  std::optional<Vec3> before, after;
};
struct DiffOptions {
  // Budgets bound visited union objects/properties, not just changed rows.
  std::size_t max_objects = 10000, max_properties = 100000,
              max_auxiliary_changes = 10000;
  std::size_t max_retained_value_bytes = 64u * 1024u * 1024u;
  bool include_lifecycle_properties = false;
  std::function<bool()> cancelled;
  CoordinateSpace coordinate_space = CoordinateSpace::native;
};
struct StateDiff {
  std::size_t before_event_count = 0, after_event_count = 0;
  std::vector<ObjectChange> objects;
  std::vector<CameraChange> cameras;
  std::vector<OriginShiftChange> origin_shifts;
  bool complete = true, cancelled = false, truncated = false;
  std::vector<std::string> diagnostics;
  CoordinateSpace coordinate_space = CoordinateSpace::native;
};
// Snapshot state only; metadata definition history is not fabricated. ID reuse
// produces destroyed(old generation) plus created(new generation), never a
// merge.
PVD_ANALYSIS_API StateDiff diff_snapshots(const Snapshot &, const Snapshot &,
                                          DiffOptions options = {});
PVD_ANALYSIS_API StateDiff diff_events(const Session &,
                                       std::size_t before_event_count,
                                       std::size_t after_event_count,
                                       DiffOptions options = {});
PVD_ANALYSIS_API StateDiff diff_frames(const Session &,
                                       const FramePoint &before,
                                       const FramePoint &after,
                                       DiffOptions options = {});
} // namespace pvd::analysis
