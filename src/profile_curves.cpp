#include "pvd/profile_curves.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>

namespace pvd::analysis {
namespace {
struct Sum {
  std::uint64_t low = 0, high = 0;
  void add(std::uint64_t value) {
    const auto old = low;
    low += value;
    if (low < old)
      ++high;
  }
  long double approximate() const {
    return std::ldexp(static_cast<long double>(high), 64) + low;
  }
  std::string decimal() const {
    std::array<std::uint32_t, 4> words{static_cast<std::uint32_t>(high >> 32),
                                       static_cast<std::uint32_t>(high),
                                       static_cast<std::uint32_t>(low >> 32),
                                       static_cast<std::uint32_t>(low)};
    std::string result;
    do {
      std::uint64_t remainder = 0;
      for (auto &word : words) {
        const auto v = (remainder << 32) | word;
        word = static_cast<std::uint32_t>(v / 10);
        remainder = v % 10;
      }
      result.push_back(static_cast<char>('0' + remainder));
    } while (
        std::any_of(words.begin(), words.end(), [](auto n) { return n != 0; }));
    std::reverse(result.begin(), result.end());
    return result;
  }
};
struct Aggregate {
  Sum total;
  std::uint64_t count = 0;
  std::optional<std::uint64_t> minimum, maximum;
  void add(std::uint64_t ticks) {
    total.add(ticks);
    ++count;
    minimum = minimum ? std::min(*minimum, ticks) : ticks;
    maximum = maximum ? std::max(*maximum, ticks) : ticks;
  }
};
bool nanosecond_metric(ProfileMetric metric) {
  return metric >= ProfileMetric::total_nanoseconds;
}
bool filter_zone(const ProfileCurveFilter &f, profile::ZoneKey zone) {
  return !f.zone || *f.zone == zone;
}
bool match(const profile::Index &index, const ProfileCurveFilter &f,
           const profile::Interval &span) {
  if (!filter_zone(f, span.start_source.zone) ||
      (f.event_id && *f.event_id != span.event_id) ||
      (f.thread_id && *f.thread_id != span.thread_id) ||
      (f.context_id && *f.context_id != span.context_id))
    return false;
  return f.text.empty() ||
         index.zone_name(span.start_source.zone).find(f.text) !=
             std::string::npos ||
         index.event_name(span.start_source.zone, span.event_id).find(f.text) !=
             std::string::npos;
}
bool critical(profile::DiagnosticCode code) {
  using C = profile::DiagnosticCode;
  switch (code) {
  case C::malformed_packet:
  case C::truncated_packet:
  case C::unknown_event:
  case C::relative_without_context:
  case C::timestamp_overflow:
  case C::backwards_interval:
  case C::unmatched_stop:
  case C::unclosed_start:
  case C::abandoned_start:
  case C::resource_limit:
    return true;
  default:
    return false;
  }
}
std::string quote(const std::string &value) {
  std::string r = "\"";
  for (char c : value) {
    if (c == '\"')
      r += '\"';
    r += c;
  }
  return r + '\"';
}
std::size_t choose(std::size_t i, std::size_t total, std::size_t keep) {
  if (keep <= 1)
    return 0;
  const auto numerator = total - 1, denominator = keep - 1;
  // Exact without size_t product overflow; i <= denominator <= max_samples.
  return (numerator / denominator) * i +
         static_cast<std::size_t>((std::uint64_t(numerator % denominator) * i) /
                                  denominator);
}
} // namespace
const char *profile_metric_name(ProfileMetric metric) noexcept {
  switch (metric) {
  case ProfileMetric::count:
    return "Completed scope count";
  case ProfileMetric::total_ticks:
    return "Inclusive total duration (ticks)";
  case ProfileMetric::mean_ticks:
    return "Mean completed duration (ticks)";
  case ProfileMetric::minimum_ticks:
    return "Minimum completed duration (ticks)";
  case ProfileMetric::maximum_ticks:
    return "Maximum completed duration (ticks)";
  case ProfileMetric::total_nanoseconds:
    return "Inclusive total duration (ns)";
  case ProfileMetric::mean_nanoseconds:
    return "Mean completed duration (ns)";
  case ProfileMetric::minimum_nanoseconds:
    return "Minimum completed duration (ns)";
  case ProfileMetric::maximum_nanoseconds:
    return "Maximum completed duration (ns)";
  }
  return "Unknown profile metric";
}
const char *profile_state_name(ProfileSampleState state) noexcept {
  switch (state) {
  case ProfileSampleState::observed:
    return "observed";
  case ProfileSampleState::no_matches:
    return "no_matches";
  case ProfileSampleState::not_collected:
    return "not_collected";
  case ProfileSampleState::incomplete:
    return "incomplete";
  case ProfileSampleState::invalid_clock:
    return "invalid_clock";
  }
  return "invalid_state";
}
struct ProfileCurves::Impl {
  profile::Index index;
  const Session *session = nullptr;
  StreamHeader header;
  std::size_t frame_cursor = 0, prefix = 0;
  bool source_failed = false;
  std::map<std::uint64_t, std::vector<FrameMarker>> frames;
  explicit Impl(profile::Options options) : index(StreamHeader{}, options) {}
};
ProfileCurves::ProfileCurves(profile::Options options)
    : impl_(std::make_unique<Impl>(options)) {}
ProfileCurves::ProfileCurves(const Session &session, profile::Options options)
    : ProfileCurves(options) {
  update(session);
}
ProfileCurves::~ProfileCurves() = default;
ProfileCurves::ProfileCurves(ProfileCurves &&) noexcept = default;
ProfileCurves &ProfileCurves::operator=(ProfileCurves &&) noexcept = default;
void ProfileCurves::update(const Session &session, std::size_t requested) {
  if (impl_->session && impl_->session != &session)
    throw std::invalid_argument("profile curves cannot switch bound Session");
  const auto end = requested == std::numeric_limits<std::size_t>::max()
                       ? session.event_count()
                       : requested;
  if (end > session.event_count() || end < impl_->prefix)
    throw std::out_of_range("profile curves prefix is not an append");
  if (impl_->index.statistics().finished) {
    if (end == impl_->prefix && impl_->session == &session)
      return;
    throw std::logic_error("profile curves are finished");
  }
  impl_->index.update(session, end);
  impl_->session = &session;
  impl_->header = session.header();
  impl_->prefix = end;
  const auto &frames = session.frames();
  while (impl_->frame_cursor < frames.size() &&
         frames[impl_->frame_cursor].event_count <= end) {
    const auto &f = frames[impl_->frame_cursor];
    impl_->frames[f.scene_id].push_back(f);
    ++impl_->frame_cursor;
  }
  if (end == session.event_count() &&
      (session.finished() || session.failed())) {
    impl_->source_failed = session.failed();
    impl_->index.finish();
  }
}
void ProfileCurves::finish() { impl_->index.finish(); }
std::size_t ProfileCurves::event_count() const noexcept {
  return impl_->prefix;
}
const profile::Index &ProfileCurves::profile_index() const noexcept {
  return impl_->index;
}

ProfileCurveSeries ProfileCurves::sample(ProfileCurveRequest request,
                                         ProfileCurveOptions options) const {
  if (!impl_->session)
    throw std::logic_error("update a Session before sampling profile curves");
  if (request.source.sampling != Sampling::native_frame)
    throw std::invalid_argument(
        "Profile curves require native scene frame sections; after-fetch "
        "validation sections are not simulation frame windows");
  if (!request.stride || request.first_frame > request.end_frame)
    throw std::invalid_argument("invalid profile curve frame range/stride");
  if (!options.max_samples || options.max_samples > 1000000 ||
      !options.max_work)
    throw std::invalid_argument("profile curve budgets require 1..1000000 "
                                "samples and positive max_work");
  if (static_cast<int>(request.metric) < 0 ||
      request.metric > ProfileMetric::maximum_nanoseconds ||
      static_cast<int>(request.coverage) < 0 ||
      request.coverage > ProfileCoverage::assume_continuous ||
      static_cast<int>(request.partial) < 0 ||
      request.partial > ProfilePartial::observed_subset ||
      static_cast<int>(options.resampling) < 0 ||
      options.resampling > Resampling::uniform)
    throw std::invalid_argument("invalid profile curve enum option");
  ProfileCurveSeries result;
  result.request = request;
  result.source_event_count = impl_->prefix;
  result.timestamp_numerator = impl_->header.timestamp_numerator;
  result.timestamp_denominator = impl_->header.timestamp_denominator;
  const auto stats = impl_->index.statistics();
  result.provisional = !stats.finished;
  const bool calibrated =
      impl_->header.timestamp_numerator && impl_->header.timestamp_denominator;
  if (!calibrated)
    result.diagnostics.push_back(
        "Invalid counter calibration: raw ticks/counts remain available; "
        "nanosecond metrics and time coordinates are gaps");
  if (request.coverage == ProfileCoverage::observed_only)
    result.diagnostics.push_back(
        "No collection coverage watermark exists: empty windows remain "
        "not_collected gaps; assume_continuous is an explicit caller "
        "assertion");
  if (result.provisional)
    result.diagnostics.push_back(
        "Open capture: future asynchronous flushes can revise every historical "
        "bucket; provisional observations are not final totals");
  std::size_t work = 0;
  auto more = [&]() {
    if (!result.complete)
      return false;
    if (options.cancelled && options.cancelled()) {
      result.complete = false;
      result.cancelled = true;
      return false;
    }
    if (work >= options.max_work) {
      result.complete = false;
      result.work_limited = true;
      return false;
    }
    ++work;
    return true;
  };
  auto note = [&](const std::string &text) {
    if (result.diagnostics.size() < 128)
      result.diagnostics.push_back(text);
  };
  const auto found = impl_->frames.find(request.source.scene_id);
  if (found == impl_->frames.end()) {
    note("No completed native frames in this scene at the indexed prefix");
    return result;
  }
  const auto &frames = found->second;
  const auto begin = std::min(request.first_frame, frames.size()),
             end = std::min(request.end_frame, frames.size());
  auto eligible = [&](std::size_t i) {
    return (i - begin) % request.stride == 0 &&
           (!request.source.stream_id ||
            frames[i].stream_id == *request.source.stream_id);
  };
  for (auto i = begin; i < end; ++i) {
    if (!more())
      break;
    if (eligible(i))
      ++result.candidate_samples;
  }
  if (!result.complete) {
    note(result.cancelled ? "Sampling cancelled during frame enumeration"
                          : "Work budget reached during frame enumeration");
    return result;
  }
  if (!result.candidate_samples) {
    note("No native frames match the source stream/range selection");
    return result;
  }
  if (result.candidate_samples > options.max_samples &&
      options.resampling == Resampling::reject)
    throw std::length_error("profile curve sample count exceeds budget; use "
                            "uniform resampling or narrower frame range");
  const auto keep = std::min(result.candidate_samples, options.max_samples);
  result.resampled = keep < result.candidate_samples;
  if (result.resampled)
    note("Uniform original-row selection applied; skipped rows set "
         "break_before and are never interpolated");
  result.samples.reserve(keep);
  std::optional<std::uint64_t> origin;
  std::size_t ordinal = 0, next = 0;
  for (auto i = begin; i < end && next < keep; ++i) {
    if (!more())
      break;
    if (!eligible(i))
      continue;
    if (ordinal++ != choose(next, result.candidate_samples, keep))
      continue;
    const auto &frame = frames[i];
    ProfileCurveSample row;
    row.source = {i, frame.event_count, frame.scene_id, frame.stream_id,
                  frame.end_timestamp};
    row.begin_ticks = frame.begin_timestamp;
    row.end_ticks = frame.end_timestamp;
    row.provisional = result.provisional;
    row.calibration_valid = calibrated;
    row.seconds = timestamp_seconds(impl_->header, row.end_ticks);
    if (!origin)
      origin = row.end_ticks;
    if (calibrated) {
      const long double delta =
          row.end_ticks >= *origin
              ? static_cast<long double>(row.end_ticks - *origin)
              : -static_cast<long double>(*origin - row.end_ticks);
      const auto seconds =
          static_cast<double>(delta * impl_->header.timestamp_numerator /
                              impl_->header.timestamp_denominator * 1e-9L);
      if (std::isfinite(seconds))
        row.elapsed_seconds = seconds;
    }
    row.break_before = !result.samples.empty() &&
                       i - result.samples.back().source.frame_index > 1;
    result.samples.push_back(std::move(row));
    ++next;
  }
  std::vector<Aggregate> totals(result.samples.size());
  struct Window {
    std::uint64_t begin, end, prefix_max_end;
    std::size_t row;
  };
  std::vector<Window> windows;
  windows.reserve(result.samples.size());
  for (std::size_t i = 0; i < result.samples.size(); ++i) {
    const auto &row = result.samples[i];
    if (row.begin_ticks <= row.end_ticks)
      windows.push_back({row.begin_ticks, row.end_ticks, 0, i});
    else
      note("Frame " + std::to_string(row.source.frame_index) +
           " has backwards native counter bounds");
  }
  std::sort(windows.begin(), windows.end(), [](const auto &x, const auto &y) {
    return std::tie(x.begin, x.row) < std::tie(y.begin, y.row);
  });
  std::uint64_t max_end = 0;
  for (auto &window : windows) {
    max_end = std::max(max_end, window.end);
    window.prefix_max_end = max_end;
  }
  bool known_definition = false;
  for (const auto &zone : impl_->index.zones()) {
    if (!more())
      break;
    if (!filter_zone(request.filter, zone.first))
      continue;
    for (const auto &event : zone.second.event_names) {
      if (!more())
        break;
      if ((!request.filter.event_id ||
           *request.filter.event_id == event.first) &&
          event.second.compile_time_enabled &&
          (request.filter.text.empty() ||
           zone.second.name.find(request.filter.text) != std::string::npos ||
           event.second.name.find(request.filter.text) != std::string::npos)) {
        known_definition = true;
        break;
      }
    }
    if (known_definition || !result.complete)
      break;
  }

  const auto &intervals = impl_->index.intervals();
  if (result.complete)
    for (std::size_t i = 0; i < intervals.size(); ++i) {
      if (!more())
        break;
      const auto &span = intervals[i];
      if (options.progress && (i % 256 == 0 || i + 1 == intervals.size()))
        options.progress(i + 1, intervals.size());
      if (!match(impl_->index, request.filter, span))
        continue;
      ++result.matched_intervals;
      auto at = std::upper_bound(windows.begin(), windows.end(), span.end_ticks,
                                 [](std::uint64_t tick, const Window &window) {
                                   return tick < window.begin;
                                 });
      while (at != windows.begin()) {
        --at;
        if (at->prefix_max_end <= span.end_ticks)
          break;
        if (!more())
          break;
        if (at->begin <= span.end_ticks && span.end_ticks < at->end)
          totals[at->row].add(span.duration_ticks);
      }
      if (!result.complete)
        break;
    }
  bool compromised = impl_->source_failed || stats.saturated ||
                     stats.suppressed_diagnostics > 0;
  std::size_t issues = 0;
  for (const auto &diagnostic : impl_->index.diagnostics()) {
    if (!more())
      break;
    if (critical(diagnostic.code) &&
        filter_zone(request.filter, diagnostic.source.zone)) {
      compromised = true;
      if (issues++ < 8)
        note("Profile data incomplete: " + diagnostic.message);
    }
  }

  if (issues > 8)
    note(std::to_string(issues - 8) +
         " further timing integrity diagnostics affect this selection");
  if (stats.saturated || stats.suppressed_diagnostics)
    note("Profile index reached a resource/diagnostic budget; absence is not "
         "evidence of zero events");
  if (result.cancelled)
    note("Sampling cancelled; raw aggregates reflect only visited received "
         "intervals");
  if (result.work_limited)
    note("Work budget reached; raw aggregates reflect only visited received "
         "intervals");
  const bool partial = result.provisional || compromised || !result.complete;
  const bool recognized = known_definition || result.matched_intervals > 0;
  for (std::size_t i = 0; i < result.samples.size(); ++i) {
    auto &row = result.samples[i];
    const auto &aggregate = totals[i];
    row.completed_count = aggregate.count;
    row.total_ticks = aggregate.total.decimal();
    row.minimum_ticks = aggregate.minimum;
    row.maximum_ticks = aggregate.maximum;
    if (aggregate.count)
      row.mean_ticks =
          static_cast<double>(aggregate.total.approximate() / aggregate.count);
    if (row.end_ticks < row.begin_ticks ||
        (!calibrated && nanosecond_metric(request.metric))) {
      row.state = ProfileSampleState::invalid_clock;
      continue;
    }
    const bool no_observations = !aggregate.count;
    if (partial && (recognized || compromised || !result.complete))
      row.state = ProfileSampleState::incomplete;
    else if (aggregate.count)
      row.state = ProfileSampleState::observed;
    else if (request.coverage == ProfileCoverage::assume_continuous &&
             recognized)
      row.state = ProfileSampleState::no_matches;
    else
      row.state = ProfileSampleState::not_collected;
    if (row.state == ProfileSampleState::not_collected ||
        (row.state == ProfileSampleState::incomplete &&
         request.partial == ProfilePartial::gap))
      continue;
    const long double total = aggregate.total.approximate();
    const auto floating = [&](long double n) {
      const auto value = static_cast<double>(n);
      if (std::isfinite(value)) {
        row.value = Numeric{value};
        row.exact_value = numeric_text(*row.value);
      }
    };
    switch (request.metric) {
    case ProfileMetric::count:
      row.value = Numeric{aggregate.count};
      row.exact_value = std::to_string(aggregate.count);
      break;
    case ProfileMetric::total_ticks:
      row.value = aggregate.total.high ? Numeric{static_cast<double>(total)}
                                       : Numeric{aggregate.total.low};
      row.exact_value = row.total_ticks;
      break;
    case ProfileMetric::mean_ticks:
      if (aggregate.count) {
        row.value = Numeric{*row.mean_ticks};
        row.exact_value =
            row.total_ticks + "/" + std::to_string(aggregate.count);
      }
      break;
    case ProfileMetric::minimum_ticks:
      if (aggregate.minimum) {
        row.value = Numeric{*aggregate.minimum};
        row.exact_value = std::to_string(*aggregate.minimum);
      }
      break;
    case ProfileMetric::maximum_ticks:
      if (aggregate.maximum) {
        row.value = Numeric{*aggregate.maximum};
        row.exact_value = std::to_string(*aggregate.maximum);
      }
      break;
    case ProfileMetric::total_nanoseconds:
      floating(total * impl_->header.timestamp_numerator /
               impl_->header.timestamp_denominator);
      break;
    case ProfileMetric::mean_nanoseconds:
      if (!no_observations)
        floating(total / aggregate.count * impl_->header.timestamp_numerator /
                 impl_->header.timestamp_denominator);
      break;
    case ProfileMetric::minimum_nanoseconds:
      if (aggregate.minimum)
        floating(static_cast<long double>(*aggregate.minimum) *
                 impl_->header.timestamp_numerator /
                 impl_->header.timestamp_denominator);
      break;
    case ProfileMetric::maximum_nanoseconds:
      if (aggregate.maximum)
        floating(static_cast<long double>(*aggregate.maximum) *
                 impl_->header.timestamp_numerator /
                 impl_->header.timestamp_denominator);
      break;
    }
  }
  return result;
}

void write_profile_curve_csv(std::ostream &output,
                             const ProfileCurveSeries &series) {
  output << "scene_id,stream_id,frame_index,event_count,begin_ticks,end_ticks,"
            "completed_count,total_ticks,minimum_ticks,maximum_ticks,metric,"
            "value_exact,value_plot,state,provisional,calibration_valid,break_"
            "before,coverage,partial_policy,analysis_event_count,series_"
            "complete,cancelled,resampled,work_limited,zone_id,zone_generation,"
            "event_id,thread_id,context_id,text_filter,timestamp_numerator,"
            "timestamp_denominator,attribution\r\n";
  for (const auto &sample : series.samples) {
    std::ostringstream row;
    row.imbue(std::locale::classic());
    row << sample.source.scene_id << ',' << sample.source.stream_id << ','
        << sample.source.frame_index << ',' << sample.source.event_count << ','
        << sample.begin_ticks << ',' << sample.end_ticks << ','
        << sample.completed_count << ',' << sample.total_ticks << ',';
    if (sample.minimum_ticks)
      row << *sample.minimum_ticks;
    row << ',';
    if (sample.maximum_ticks)
      row << *sample.maximum_ticks;
    row << ',' << quote(profile_metric_name(series.request.metric)) << ','
        << quote(sample.exact_value) << ',';
    if (sample.value)
      row << numeric_text(*sample.value);
    row << ',' << profile_state_name(sample.state) << ',' << sample.provisional
        << ',' << sample.calibration_valid << ',' << sample.break_before << ','
        << (series.request.coverage == ProfileCoverage::assume_continuous
                ? "assume_continuous"
                : "observed_only")
        << ','
        << (series.request.partial == ProfilePartial::observed_subset
                ? "observed_subset"
                : "gap")
        << ',' << series.source_event_count << ',' << series.complete << ','
        << series.cancelled << ',' << series.resampled << ','
        << series.work_limited << ',';
    const auto &f = series.request.filter;
    if (f.zone)
      row << f.zone->id;
    row << ',';
    if (f.zone)
      row << f.zone->generation;
    row << ',';
    if (f.event_id)
      row << *f.event_id;
    row << ',';
    if (f.thread_id)
      row << *f.thread_id;
    row << ',';
    if (f.context_id)
      row << *f.context_id;
    row << ',' << quote(f.text) << ',' << series.timestamp_numerator << ','
        << series.timestamp_denominator << ',' << quote(series.attribution)
        << "\r\n";
    output << row.str();
  }
  if (!output)
    throw std::runtime_error("profile curve CSV write failed");
}
} // namespace pvd::analysis
