#include "pvd/frame_timing.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <numeric>
#include <ostream>
#include <sstream>

namespace pvd::analysis {
namespace {
std::string product(std::uint64_t a, std::uint64_t b) {
  const auto a0 = std::uint64_t(std::uint32_t(a)), a1 = a >> 32;
  const auto b0 = std::uint64_t(std::uint32_t(b)), b1 = b >> 32;
  const auto p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  const auto middle = (p00 >> 32) + std::uint32_t(p01) + std::uint32_t(p10);
  const auto low = (middle << 32) | std::uint32_t(p00);
  const auto high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
  std::array<std::uint32_t, 4> words{
      std::uint32_t(high >> 32), std::uint32_t(high), std::uint32_t(low >> 32),
      std::uint32_t(low)};
  std::string text;
  do {
    std::uint64_t remainder = 0;
    for (auto &word : words) {
      const auto v = (remainder << 32) | word;
      word = std::uint32_t(v / 10);
      remainder = v % 10;
    }
    text.push_back(char('0' + remainder));
  } while (
      std::any_of(words.begin(), words.end(), [](auto n) { return n != 0; }));
  std::reverse(text.begin(), text.end());
  return text;
}
void rational(FrameTimingSample &s, std::uint64_t ticks,
              std::uint64_t numerator, std::uint64_t denominator) {
  auto gcd = std::gcd(ticks, denominator);
  ticks /= gcd;
  denominator /= gcd;
  gcd = std::gcd(numerator, denominator);
  numerator /= gcd;
  denominator /= gcd;
  s.nanoseconds_numerator = product(ticks, numerator);
  s.nanoseconds_denominator = denominator;
  s.exact_nanoseconds = s.nanoseconds_numerator;
  if (denominator != 1)
    s.exact_nanoseconds += "/" + std::to_string(denominator);
}
std::size_t choose(std::size_t i, std::size_t count, std::size_t keep) {
  if (keep <= 1)
    return 0;
  const auto n = count - 1, d = keep - 1;
  return (n / d) * i + std::size_t((std::uint64_t(n % d) * i) / d);
}
} // namespace
const char *frame_timing_state_name(FrameTimingState state) noexcept {
  switch (state) {
  case FrameTimingState::valid:
    return "valid";
  case FrameTimingState::backwards_clock:
    return "backwards_clock";
  case FrameTimingState::invalid_calibration:
    return "invalid_calibration";
  }
  return "unknown";
}
FrameTimingSeries sample_frame_timing(const Session &session,
                                      FrameTimingRequest request,
                                      FrameTimingOptions options) {
  if (request.source.sampling != Sampling::native_frame)
    throw std::invalid_argument(
        "frame timing requires native frame sections; after-fetch intervals "
        "are not frame durations");
  if (request.metric != FrameTimingMetric::ticks &&
      request.metric != FrameTimingMetric::nanoseconds)
    throw std::invalid_argument("invalid frame timing metric");
  if (!request.stride || request.first_frame > request.end_frame ||
      !options.max_samples || options.max_samples > 1000000 ||
      !options.max_work)
    throw std::invalid_argument(
        "invalid frame timing range/stride/resource budget");
  if (options.resampling != Resampling::uniform &&
      options.resampling != Resampling::reject)
    throw std::invalid_argument("invalid frame timing resampling");
  const auto end =
      options.event_count == std::numeric_limits<std::size_t>::max()
          ? session.event_count()
          : options.event_count;
  if (end > session.event_count())
    throw std::out_of_range(
        "frame timing prefix exceeds accepted Session events");
  FrameTimingSeries result;
  result.request = request;
  result.source_event_count = end;
  const auto &header = session.header();
  result.timestamp_numerator = header.timestamp_numerator;
  result.timestamp_denominator = header.timestamp_denominator;
  const bool calibrated =
      header.timestamp_numerator && header.timestamp_denominator;
  if (!calibrated)
    result.diagnostics.emplace_back(
        "Invalid native counter calibration: raw duration ticks remain "
        "available; nanosecond/time values are gaps");
  if (!session.finished())
    result.diagnostics.emplace_back(
        "Only completed accepted native sections are included; the current "
        "unclosed frame is excluded");
  std::size_t work = 0;
  auto allowed = [&] {
    if (options.cancelled && options.cancelled()) {
      result.cancelled = true;
      result.complete = false;
      return false;
    }
    if (work >= options.max_work) {
      result.work_limited = true;
      result.complete = false;
      if (result.diagnostics.empty() ||
          result.diagnostics.back() !=
              "Native frame timing work budget reached")
        result.diagnostics.emplace_back(
            "Native frame timing work budget reached");
      return false;
    }
    ++work;
    return true;
  };
  auto visit = [&](auto &&visitor) {
    std::size_t ordinal = 0;
    for (const auto &frame : session.frames()) {
      if (frame.event_count > end)
        break;
      if (!allowed())
        return false;
      if (frame.scene_id != request.source.scene_id)
        continue;
      const auto index = ordinal++;
      if (index < request.first_frame || index >= request.end_frame ||
          (index - request.first_frame) % request.stride ||
          (request.source.stream_id &&
           *request.source.stream_id != frame.stream_id))
        continue;
      if (!visitor(frame, index))
        break;
    }
    return true;
  };
  if (!visit([&](const auto &, std::size_t) {
        ++result.candidate_samples;
        return true;
      }))
    return result;
  if (options.resampling == Resampling::reject &&
      result.candidate_samples > options.max_samples)
    throw std::length_error("native frame timing sample budget exceeded");
  const auto keep = std::min(result.candidate_samples, options.max_samples);
  result.resampled = keep < result.candidate_samples;
  result.samples.reserve(keep);
  if (!keep)
    return result;
  std::size_t candidate = 0, next = 0;
  std::optional<std::uint64_t> time_origin;
  visit([&](const FrameMarker &frame, std::size_t index) {
    if (candidate++ != choose(next, result.candidate_samples, keep))
      return true;
    FrameTimingSample row;
    row.source = {index, frame.event_count, frame.scene_id, frame.stream_id,
                  frame.end_timestamp};
    row.begin_ticks = frame.begin_timestamp;
    row.end_ticks = frame.end_timestamp;
    row.calibration_valid = calibrated;
    row.break_before = !result.samples.empty() &&
                       index - result.samples.back().source.frame_index > 1;
    row.seconds = timestamp_seconds(header, frame.end_timestamp);
    if (!time_origin)
      time_origin = frame.end_timestamp;
    if (calibrated) {
      const long double delta =
          frame.end_timestamp >= *time_origin
              ? static_cast<long double>(frame.end_timestamp - *time_origin)
              : -static_cast<long double>(*time_origin - frame.end_timestamp);
      const auto seconds =
          static_cast<double>(delta * header.timestamp_numerator /
                              header.timestamp_denominator * 1e-9L);
      if (std::isfinite(seconds))
        row.elapsed_seconds = seconds;
    }
    if (frame.end_timestamp < frame.begin_timestamp)
      row.state = FrameTimingState::backwards_clock;
    else {
      row.duration_ticks = frame.end_timestamp - frame.begin_timestamp;
      if (calibrated)
        rational(row, *row.duration_ticks, header.timestamp_numerator,
                 header.timestamp_denominator);
      if (request.metric == FrameTimingMetric::ticks) {
        row.value = Numeric{*row.duration_ticks};
        row.exact_value = std::to_string(*row.duration_ticks);
      } else if (!calibrated)
        row.state = FrameTimingState::invalid_calibration;
      else {
        const auto value = static_cast<double>(
            static_cast<long double>(*row.duration_ticks) *
            header.timestamp_numerator / header.timestamp_denominator);
        if (std::isfinite(value)) {
          row.value = Numeric{value};
          row.exact_value = row.exact_nanoseconds;
        }
      }
    }
    result.samples.push_back(std::move(row));
    ++next;
    if (options.progress)
      options.progress(result.samples.size(), keep);
    return next < keep;
  });
  return result;
}
void write_frame_timing_csv(std::ostream &out,
                            const FrameTimingSeries &series) {
  out << "frame_index,scene_id,stream_id,event_count,begin_ticks,end_ticks,"
         "duration_ticks,nanoseconds_numerator,nanoseconds_denominator,exact_"
         "nanoseconds,metric,exact_value,plot_value,time_seconds,elapsed_"
         "seconds,state,calibration_valid,break_before,timestamp_numerator,"
         "timestamp_denominator,source_event_count,series_complete,series_"
         "cancelled,series_resampled,work_limited\r\n";
  for (const auto &s : series.samples) {
    std::ostringstream row;
    row.imbue(std::locale::classic());
    row << s.source.frame_index << ',' << s.source.scene_id << ','
        << s.source.stream_id << ',' << s.source.event_count << ','
        << s.begin_ticks << ',' << s.end_ticks << ',';
    if (s.duration_ticks)
      row << *s.duration_ticks;
    row << ',' << s.nanoseconds_numerator << ',';
    if (s.nanoseconds_denominator)
      row << s.nanoseconds_denominator;
    row << ',' << s.exact_nanoseconds << ','
        << (series.request.metric == FrameTimingMetric::ticks
                ? "native_frame_ticks"
                : "native_frame_nanoseconds")
        << ',' << s.exact_value << ',';
    if (s.value)
      row << numeric_text(*s.value);
    row << ',';
    if (s.seconds)
      row << numeric_text(Numeric{*s.seconds});
    row << ',';
    if (s.elapsed_seconds)
      row << numeric_text(Numeric{*s.elapsed_seconds});
    row << ',' << frame_timing_state_name(s.state) << ',' << s.calibration_valid
        << ',' << s.break_before << ',' << series.timestamp_numerator << ','
        << series.timestamp_denominator << ',' << series.source_event_count
        << ',' << series.complete << ',' << series.cancelled << ','
        << series.resampled << ',' << series.work_limited << "\r\n";
    out << row.str();
  }
  if (!out)
    throw std::runtime_error("frame timing CSV output failed");
}
} // namespace pvd::analysis
