#pragma once
#include "pvd/curves.hpp"

namespace pvd::analysis {
enum class FrameTimingMetric { ticks, nanoseconds };
enum class FrameTimingState { valid, backwards_clock, invalid_calibration };
struct FrameTimingRequest {
  FrameSource source; // Native "frame" sections only; after_fetch is rejected.
  FrameTimingMetric metric = FrameTimingMetric::nanoseconds;
  std::size_t first_frame = 0,
              end_frame = std::numeric_limits<std::size_t>::max(), stride = 1;
};
struct FrameTimingOptions {
  std::size_t max_samples = 2000, max_work = 20000000;
  // Bound to an accepted prefix; excludes frames whose EndSection is later.
  std::size_t event_count = std::numeric_limits<std::size_t>::max();
  Resampling resampling = Resampling::uniform;
  std::function<bool()> cancelled;
  std::function<void(std::size_t completed, std::size_t total)> progress;
};
struct FrameTimingSample {
  SampleLocation source;
  std::uint64_t begin_ticks = 0, end_ticks = 0;
  std::optional<std::uint64_t> duration_ticks;
  // Exact reduced nanosecond rational; the numerator can require 128 bits.
  std::string nanoseconds_numerator;
  std::uint64_t nanoseconds_denominator = 0;
  std::string exact_nanoseconds, exact_value;
  std::optional<Numeric> value; // Plot conversion may round; CSV stays exact.
  std::optional<double> seconds, elapsed_seconds;
  FrameTimingState state = FrameTimingState::valid;
  bool break_before = false, calibration_valid = true;
};
struct FrameTimingSeries {
  FrameTimingRequest request;
  std::uint64_t timestamp_numerator = 0, timestamp_denominator = 0;
  std::size_t source_event_count = 0, candidate_samples = 0;
  std::vector<FrameTimingSample> samples;
  std::vector<std::string> diagnostics;
  bool complete = true, cancelled = false, resampled = false,
       work_limited = false;
};
// Wall-clock native section duration, not CPU scope sums, receipt intervals,
// after-fetch windows, simulation dt or playback FPS. No object snapshots or
// profile packet decoding. Only completed accepted frame markers are sampled.
PVD_ANALYSIS_API FrameTimingSeries sample_frame_timing(const Session &,
                                                       FrameTimingRequest,
                                                       FrameTimingOptions = {});
PVD_ANALYSIS_API void write_frame_timing_csv(std::ostream &,
                                             const FrameTimingSeries &);
PVD_ANALYSIS_API const char *frame_timing_state_name(FrameTimingState) noexcept;
} // namespace pvd::analysis
