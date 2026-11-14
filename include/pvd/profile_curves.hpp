#pragma once
#include "pvd/curves.hpp"
#include "pvd/frame_timing.hpp"
#include "pvd/profile.hpp"

namespace pvd::analysis {
enum class ProfileMetric {
  count,
  total_ticks,
  mean_ticks,
  minimum_ticks,
  maximum_ticks,
  total_nanoseconds,
  mean_nanoseconds,
  minimum_nanoseconds,
  maximum_nanoseconds,
  native_frame_ticks,
  native_frame_nanoseconds
};
inline bool native_frame_metric(ProfileMetric metric) noexcept {
  return metric==ProfileMetric::native_frame_ticks||
         metric==ProfileMetric::native_frame_nanoseconds;
}
// The SDK has no collection-coverage or final-flush watermark record. Opting in
// asserts that matching CPU events were continuously collected for these
// windows.
enum class ProfileCoverage { observed_only, assume_continuous };
enum class ProfilePartial { gap, observed_subset };
enum class ProfileSampleState {
  observed,
  no_matches,
  not_collected,
  incomplete,
  invalid_clock
};
PVD_ANALYSIS_API const char *profile_metric_name(ProfileMetric) noexcept;
PVD_ANALYSIS_API const char *profile_state_name(ProfileSampleState) noexcept;
struct ProfileCurveFilter {
  std::optional<profile::ZoneKey> zone;
  std::optional<std::uint16_t> event_id;
  std::optional<std::uint32_t> thread_id;
  std::optional<std::uint64_t> context_id;
  std::string text; // Case-sensitive zone/event-name substring.
};
struct ProfileCurveRequest {
  FrameSource
      source; // Native scene frame sections only; after_fetch is rejected.
  ProfileCurveFilter filter;
  ProfileMetric metric = ProfileMetric::total_ticks;
  ProfileCoverage coverage = ProfileCoverage::observed_only;
  ProfilePartial partial = ProfilePartial::gap;
  std::size_t first_frame = 0,
              end_frame = std::numeric_limits<std::size_t>::max(), stride = 1;
};
struct ProfileCurveOptions {
  std::size_t max_samples = 2000, max_work = 20000000;
  Resampling resampling = Resampling::uniform;
  std::function<bool()> cancelled;
  std::function<void(std::size_t completed, std::size_t total)> progress;
};
struct ProfileCurveSample {
  SampleLocation source;
  std::uint64_t begin_ticks = 0, end_ticks = 0, completed_count = 0;
  // Exact decimal sum of inclusive native durations. May exceed uint64_t when
  // nested/multi-thread scopes overlap. mean is the rational total/count.
  std::string total_ticks = "0";
  std::optional<std::uint64_t> minimum_ticks, maximum_ticks;
  std::optional<double> mean_ticks, seconds, elapsed_seconds;
  std::optional<Numeric>
      value;               // Plot conversion may round; use exact_value/CSV.
  std::string exact_value; // Integers exact; mean_ticks is "total/count".
  ProfileSampleState state = ProfileSampleState::not_collected;
  bool break_before = false, provisional = false, calibration_valid = true;
};
struct ProfileCurveSeries {
  ProfileCurveRequest request;
  // Attribution is by original scope end counter in [frame.begin,frame.end),
  // not by the outer flush packet's receipt frame. Long scopes contribute their
  // full inclusive duration once per matching window; this is not CPU
  // utilization.
  std::string attribution =
      "Observed completed CPU scopes; inclusive duration by "
      "native end tick in [frame begin, frame end)";
  std::uint64_t timestamp_numerator = 0, timestamp_denominator = 0;
  std::size_t source_event_count = 0, candidate_samples = 0,
              matched_intervals = 0;
  std::vector<ProfileCurveSample> samples;
  std::vector<std::string> diagnostics;
  bool complete = true, cancelled = false, resampled = false,
       work_limited = false, provisional = false;
  // Frame metrics use marker wall time, not CPU aggregates/coverage. Kept for
  // exact frame-specific CSV and explicit consumer inspection.
  std::shared_ptr<const FrameTimingSeries> frame_timing;
};
// One incrementally updated profile Index per Session. Repeated query/draw
// calls do not parse channel history. Late flushes may revise historical frame
// buckets. Caller serializes access with Session append and Index update.
class ProfileCurves {
public:
  PVD_ANALYSIS_API explicit ProfileCurves(profile::Options = {});
  PVD_ANALYSIS_API explicit ProfileCurves(const Session &,
                                          profile::Options = {});
  PVD_ANALYSIS_API ~ProfileCurves();
  PVD_ANALYSIS_API ProfileCurves(ProfileCurves &&) noexcept;
  PVD_ANALYSIS_API ProfileCurves &operator=(ProfileCurves &&) noexcept;
  ProfileCurves(const ProfileCurves &) = delete;
  ProfileCurves &operator=(const ProfileCurves &) = delete;
  PVD_ANALYSIS_API void
  update(const Session &,
         std::size_t event_count = std::numeric_limits<std::size_t>::max());
  PVD_ANALYSIS_API void finish();
  PVD_ANALYSIS_API std::size_t event_count() const noexcept;
  PVD_ANALYSIS_API const profile::Index &profile_index() const noexcept;
  PVD_ANALYSIS_API ProfileCurveSeries sample(ProfileCurveRequest,
                                             ProfileCurveOptions = {}) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// RFC4180, classic locale. Raw ticks, counts and wide sums never pass through
// floating point. exact_value also preserves an exact mean-ticks rational.
PVD_ANALYSIS_API void write_profile_curve_csv(std::ostream &,
                                              const ProfileCurveSeries &);
} // namespace pvd::analysis
