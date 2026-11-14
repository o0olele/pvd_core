#include "pvd/frame_timing.hpp"
#include <filesystem>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace a = pvd::analysis;
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::uint64_t stream = 1;
  std::vector<std::size_t> prefixes;
  template <class T> void emit(T v) {
    events.push_back(
        {stream, 999999, events.size() * 7, Payload{std::move(v)}});
  }
  Fixture() {
    header.timestamp_numerator = 7;
    header.timestamp_denominator = 3;
    emit(StringHandleEvent{"test", 1});
    emit(StringHandleEvent{"Scene", 2});
    emit(StringHandleEvent{"frame", 3});
    emit(StringHandleEvent{"validation_after_fetch", 4});
    emit(CreateClass{{1, 2}});
    emit(CreateInstance{{1, 2}, 1});
    emit(CreateInstance{{1, 2}, 2});
    stream = 2;
    emit(StringHandleEvent{"frame", 3});
    stream = 1;
  }
  void frame(std::uint64_t scene, std::uint64_t begin, std::uint64_t end) {
    emit(BeginSection{scene, 3, begin});
    emit(EndSection{scene, 3, end});
    if (scene == 1)
      prefixes.push_back(events.size());
  }
  void standard() {
    frame(1, 10, 15);
    frame(2, 20, 120);
    stream = 2;
    frame(1, 20, 20);
    stream = 1;
    frame(1, 100, 190);
    frame(1, 200, 199);
    emit(BeginSection{1, 4, 1000});
    emit(EndSection{1, 4, 1007});
    emit(BeginSection{1, 3, 300});
  }
};
void basic() {
  Fixture f;
  f.standard();
  Session s(f.header, f.events);
  a::FrameTimingRequest q;
  q.source.scene_id = 1;
  auto v = a::sample_frame_timing(s, q);
  check(v.samples.size() == 4 && v.candidate_samples == 4,
        "native-only scene frame count");
  check(v.samples[0].duration_ticks == 5 &&
            v.samples[0].exact_nanoseconds == "35/3" &&
            v.samples[0].nanoseconds_numerator == "35" &&
            v.samples[0].nanoseconds_denominator == 3,
        "independent exact fractional calibration oracle");
  check(v.samples[1].duration_ticks == 0 && v.samples[1].value &&
            v.samples[1].exact_nanoseconds == "0",
        "zero-duration native frame must be valid zero");
  check(v.samples[2].duration_ticks == 90 && v.samples[2].exact_value == "210",
        "native duration is not receipt delta or after-fetch interval");
  check(v.samples[3].state == a::FrameTimingState::backwards_clock &&
            !v.samples[3].value && !v.samples[3].duration_ticks,
        "backwards counter must be a gap");
  q.source.stream_id = 1;
  auto filtered = a::sample_frame_timing(s, q);
  check(filtered.samples.size() == 3 &&
            filtered.samples[1].source.frame_index == 2 &&
            filtered.samples[1].break_before,
        "stream filter lost scene ordinal/gap");
  q.source.stream_id.reset();
  a::FrameTimingOptions opts;
  opts.max_samples = 2;
  auto limited = a::sample_frame_timing(s, q, opts);
  check(limited.resampled && limited.samples.size() == 2 &&
            limited.samples.back().source.frame_index == 3 &&
            limited.samples.back().break_before &&
            !limited.samples.back().value,
        "resampling must retain original gap rows");
  opts.resampling = a::Resampling::reject;
  bool rejected = false;
  try {
    a::sample_frame_timing(s, q, opts);
  } catch (const std::length_error &) {
    rejected = true;
  }
  check(rejected, "reject sample budget");
  opts = {};
  opts.event_count = f.prefixes[0];
  check(a::sample_frame_timing(s, q, opts).samples.size() == 1,
        "future frame leaked beyond accepted analysis prefix");
  opts = {};
  opts.max_work = 1;
  auto work = a::sample_frame_timing(s, q, opts);
  check(!work.complete && work.work_limited, "work budget not reported");
  opts = {};
  bool stop = false;
  opts.cancelled = [&] { return stop; };
  opts.progress = [&](std::size_t, std::size_t) { stop = true; };
  auto cancelled = a::sample_frame_timing(s, q, opts);
  check(cancelled.cancelled && !cancelled.complete &&
            cancelled.samples.size() == 1,
        "cancellation lost valid sampled prefix");
  q.source.sampling = a::Sampling::after_fetch;
  rejected = false;
  try {
    a::sample_frame_timing(s, q);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  check(rejected, "after-fetch accepted as frame wall time");
  std::ostringstream csv;
  csv << std::hex;
  a::write_frame_timing_csv(csv, v);
  check(csv.str().find(",10,15,5,35,3,35/3,") != std::string::npos,
        "CSV exact duration rational / decimal despite caller hex");
  auto invalid_header = f.header;
  invalid_header.timestamp_denominator = 0;
  Session invalid(invalid_header, f.events);
  q = {};
  q.source.scene_id = 1;
  auto ns = a::sample_frame_timing(invalid, q);
  check(ns.samples[0].duration_ticks == 5 && !ns.samples[0].value &&
            ns.samples[0].state == a::FrameTimingState::invalid_calibration,
        "invalid calibration faked ns value");
  q.metric = a::FrameTimingMetric::ticks;
  auto ticks = a::sample_frame_timing(invalid, q);
  check(ticks.samples[0].value &&
            std::get<std::uint64_t>(*ticks.samples[0].value) == 5 &&
            !ticks.samples[0].calibration_valid,
        "invalid calibration erased valid raw ticks");
}
void wide() {
  Fixture f;
  f.header.timestamp_numerator = UINT64_MAX;
  f.header.timestamp_denominator = 1;
  f.frame(1, 0, UINT64_MAX);
  Session s(f.header, f.events);
  a::FrameTimingRequest q;
  q.source.scene_id = 1;
  auto value = a::sample_frame_timing(s, q);
  check(value.samples[0].exact_nanoseconds ==
            "340282366920938463426481119284349108225",
        "independent 128-bit product oracle");
  q.metric = a::FrameTimingMetric::ticks;
  auto ticks = a::sample_frame_timing(s, q);
  check(std::get<std::uint64_t>(*ticks.samples[0].value) == UINT64_MAX,
        "u64 duration lost precision");
  std::ostringstream csv;
  a::write_frame_timing_csv(csv, value);
  check(csv.str().find("18446744073709551615") != std::string::npos &&
            csv.str().find("340282366920938463426481119284349108225") !=
                std::string::npos,
        "wide CSV lost exact integers");
  Fixture g;
  g.header.timestamp_numerator = 100;
  g.header.timestamp_denominator = 1;
  g.frame(1, UINT64_MAX - 10, UINT64_MAX - 9);
  g.frame(1, UINT64_MAX - 9, UINT64_MAX - 8);
  Session adjacent(g.header, g.events);
  auto rows = a::sample_frame_timing(adjacent, q);
  check(rows.samples[1].elapsed_seconds &&
            std::abs(*rows.samples[1].elapsed_seconds - 1e-7) < 1e-20,
        "time axis erased adjacent high counters");
}
std::vector<std::string> split(const std::string &s) {
  std::vector<std::string> r;
  std::istringstream in(s);
  std::string p;
  while (std::getline(in, p, ','))
    r.push_back(p);
  return r;
}
void sdk(const char *capture, const char *oracle) {
  auto s = Session::load_file(capture);
  std::ifstream input(std::filesystem::u8path(oracle));
  check(bool(input), "SDK windows oracle");
  std::string line;
  std::getline(input, line);
  std::size_t checked = 0;
  while (std::getline(input, line)) {
    auto r = split(line);
    check(r.size() == 6, "SDK QPC bracket schema");
    a::FrameTimingRequest q;
    q.source.scene_id = std::stoull(r[1]);
    auto v = a::sample_frame_timing(s, q);
    const auto &row = v.samples.at(std::stoull(r[0]));
    const auto bl = std::stoull(r[2]), bu = std::stoull(r[3]),
               el = std::stoull(r[4]), eu = std::stoull(r[5]);
    check(row.begin_ticks >= bl && row.begin_ticks <= bu &&
              row.end_ticks >= el && row.end_ticks <= eu,
          "native frame timestamps outside independently bracketed SDK QPC");
    check(row.duration_ticks && *row.duration_ticks >= el - bu &&
              *row.duration_ticks <= eu - bl,
          "native frame duration outside independent SDK duration bounds");
    ++checked;
  }
  check(checked == 6, "SDK frame oracle case count");
  std::cout << "frame timing SDK: 6 independent QPC-bracketed native windows "
               "passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    basic();
    wide();
    if (argc == 3)
      sdk(argv[1], argv[2]);
    std::cout << "frame timing: "
                 "raw/rational/wide/zero/backwards/calibration/prefix/budgets/"
                 "CSV passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
