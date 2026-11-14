#include "pvd/profile_curves.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
using namespace pvd;
namespace a = pvd::analysis;
namespace {
void check(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
void near(double x, double y, const char *m) {
  check(std::isfinite(x) && std::abs(x - y) < 1e-8, m);
}
template <class F> void rejects(F &&f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, "expected invalid profile curve request rejection");
}
struct Wire {
  Bytes bytes;
  ByteOrder order;
  explicit Wire(ByteOrder o = ByteOrder::little) : order(o) {}
  void u(std::uint64_t n, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::uint8_t>(
          n >> ((order == ByteOrder::little ? i : width - 1 - i) * 8)));
  }
  void event(unsigned tag, std::uint64_t tick, std::uint16_t id = 1,
             std::uint32_t thread = 7, std::uint64_t context = 777) {
    u(tag, 1);
    u(15, 1);
    u(id, 2);
    u(thread, 4);
    u(context, 8);
    u(0, 1);
    u(0, 1);
    u(tick, 8);
  }
  void span(std::uint64_t begin, std::uint64_t end, std::uint16_t id = 1,
            std::uint32_t thread = 7) {
    event(1, begin, id, thread);
    event(2, end, id, thread);
  }
};
struct Fixture {
  StreamHeader header;
  std::uint64_t stream = 1;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  std::size_t late_prefix = 0;
  void emit(Payload p) {
    events.push_back({stream, events.size(), events.size() * 7, std::move(p)});
  }
  std::uint32_t text(const std::string &s) {
    auto f = handles.find(s);
    if (f != handles.end())
      return f->second;
    auto h = static_cast<std::uint32_t>(handles.size() + 1);
    handles[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *ns, const char *n) {
    auto a = text(ns), b = text(n);
    return {a, b};
  }
  void announce() {
    emit(AddProfileZone{10, "Curve, \"zone\""});
    emit(CreateInstance{type("_debugger_", "PxProfileZone"), 10});
    emit(AddProfileZoneEvent{10, "Work", 1, true});
    emit(AddProfileZoneEvent{10, "Cross-frame", 2, true});
    emit(AddProfileZoneEvent{10, "Never runs", 3, true});
    emit(AddProfileZoneEvent{10, "Disabled", 4, false});
  }
  void packet(Bytes b) {
    auto name = text("events");
    auto dt = type("physx3", "PvdU8");
    emit(SetPropertyValue{10, name, dt, static_cast<std::uint32_t>(b.size()),
                          std::move(b)});
  }
  void mark(bool begin, std::uint64_t tick, std::uint64_t scene = 1) {
    auto h = text("frame");
    if (begin)
      emit(BeginSection{scene, h, tick});
    else
      emit(EndSection{scene, h, tick});
  }
  void frame(std::uint64_t begin, std::uint64_t end, std::uint64_t scene = 1) {
    mark(true, begin, scene);
    mark(false, end, scene);
  }
  explicit Fixture(ByteOrder order = ByteOrder::little, bool populate = true) {
    header.byte_order = order;
    header.timestamp_numerator = 10;
    header.timestamp_denominator = 2;
    auto cls = type("_debugger_", "PxProfileZone");
    emit(CreateClass{cls});
    auto n = text("events"), semantic = text("profile event stream");
    auto dt = type("physx3", "PvdU8");
    emit(CreateProperty{cls, n, semantic, dt, 2, {}});
    announce();
    text("frame");
    if (!populate)
      return;
    frame(100, 200);
    Wire start(order);
    start.event(1, 100, 2);
    packet(start.bytes);
    frame(150, 250, 2);
    frame(200, 300);
    frame(300, 400);
    frame(400, 500);
    late_prefix = events.size();
    Wire late(order);
    late.event(2, 450, 2);
    late.span(110, 150);
    late.span(150, 250, 2);
    late.span(200, 200);
    late.span(210, 250, 1, 8);
    late.span(205, 255);
    late.span(480, 500);
    packet(late.bytes);
  }
  Session session() { return Session(header, events); }
};
a::ProfileCurveRequest
request(a::ProfileMetric metric = a::ProfileMetric::total_ticks) {
  a::ProfileCurveRequest r;
  r.source.scene_id = 1;
  r.metric = metric;
  return r;
}
void buckets(ByteOrder order) {
  Fixture f(order);
  auto session = f.session();
  a::ProfileCurves curves(session);
  auto r = request();
  const auto s = curves.sample(r);
  check(s.samples.size() == 4 && s.complete && !s.provisional,
        "native scene windows missing");
  check(s.samples[0].completed_count == 1 && s.samples[0].total_ticks == "40",
        "late packet was attributed to receipt frame instead of scope end "
        "counter");
  check(
      s.samples[1].completed_count == 4 && s.samples[1].total_ticks == "190" &&
          s.samples[1].minimum_ticks == 0 && s.samples[1].maximum_ticks == 100,
      "cross-frame, zero, or concurrent thread aggregation wrong");
  check(
      s.samples[3].completed_count == 1 && s.samples[3].total_ticks == "350",
      "long scope was clipped or split instead of completed-scope attribution");
  check(s.samples[2].state == a::ProfileSampleState::not_collected &&
            !s.samples[2].value,
        "empty bucket fabricated collection coverage");
  r.coverage = a::ProfileCoverage::assume_continuous;
  const auto covered = curves.sample(r);
  check(covered.samples[2].state == a::ProfileSampleState::no_matches &&
            covered.samples[2].exact_value == "0",
        "explicit covered empty bucket was not zero");
  r.metric = a::ProfileMetric::mean_ticks;
  const auto mean = curves.sample(r);
  near(a::plot_value(*mean.samples[1].value), 47.5, "inclusive mean wrong");
  check(mean.samples[1].exact_value == "190/4" && !mean.samples[2].value,
        "mean lost exact rational or invented empty mean");
  r.metric = a::ProfileMetric::total_nanoseconds;
  near(a::plot_value(*curves.sample(r).samples[0].value), 200,
       "header nanoseconds per tick applied incorrectly");
  r = request();
  r.filter.event_id = 1;
  r.filter.thread_id = 8;
  const auto thread = curves.sample(r);
  check(thread.samples[1].completed_count == 1 &&
            thread.samples[1].total_ticks == "40",
        "event/thread selection ignored");
  r.filter.context_id = 999;
  check(!curves.sample(r).samples[1].value, "context selector ignored");
  r = request();
  r.source.scene_id = 2;
  const auto scene = curves.sample(r);
  check(scene.samples.size() == 1 && scene.samples[0].source.frame_index == 0 &&
            scene.samples[0].completed_count == 2,
        "overlapping scene window was treated as scope ownership");
  // Scene 2 [150,250) includes end=150 and end=200; end=250 is excluded.
  check(scene.samples[0].total_ticks == "40",
        "half-open native window boundary mismatch");
  const auto packets = curves.profile_index().statistics().profile_packets;
  curves.update(session);
  curves.sample(request());
  check(curves.profile_index().statistics().profile_packets == packets,
        "query/update reparsed indexed history");
  a::ProfileCurveOptions two;
  two.max_samples = 2;
  auto decimated = curves.sample(request(), two);
  check(decimated.resampled && decimated.samples.size() == 2 &&
            decimated.samples[1].source.frame_index == 3 &&
            decimated.samples[1].break_before,
        "uniform reduction bridged unsampled frames");
  two.resampling = a::Resampling::reject;
  rejects([&] { curves.sample(request(), two); });
  a::ProfileCurveOptions work;
  work.max_work = 1;
  auto limited = curves.sample(request(), work);
  check(limited.work_limited && !limited.complete, "work budget ignored");
  unsigned calls = 0;
  work = {};
  work.cancelled = [&] { return ++calls > 9; };
  auto cancelled = curves.sample(request(), work);
  check(cancelled.cancelled && !cancelled.complete,
        "profile curve cancellation ignored");
  auto invalid = request();
  invalid.source.sampling = a::Sampling::after_fetch;
  rejects([&] { curves.sample(invalid); });
  invalid = request();
  invalid.stride = 0;
  rejects([&] { curves.sample(invalid); });
  std::cout << "native counter bucket attribution endian="
            << (order == ByteOrder::little ? "little" : "big") << " passed\n";
}

void stream_ordinals() {
  Fixture f;
  f.stream = 2;
  f.emit(StringHandleEvent{"frame", f.handles.at("frame")});
  f.frame(100, 200);
  f.frame(200, 300);
  auto session = f.session();
  a::ProfileCurves curves(session);
  auto q = request();
  q.source.stream_id = 2;
  const auto series = curves.sample(q);
  check(series.samples.size() == 2 &&
            series.samples[0].source.frame_index == 4 &&
            series.samples[1].source.frame_index == 5,
        "stream filtering renumbered scene timeline ordinals");
  check(series.samples[0].total_ticks == "40" &&
            series.samples[1].total_ticks == "190",
        "profile channel stream was incorrectly equated with frame marker "
        "stream");
  std::size_t visited = 0, total = 0;
  a::ProfileCurveOptions progress;
  progress.progress = [&](std::size_t n, std::size_t t) {
    visited = n;
    total = t;
  };
  q.filter.thread_id = 999;
  curves.sample(q, progress);
  check(visited == total && total == 7,
        "progress omitted non-matching visited intervals");
}

void incremental_and_quality() {
  Fixture f;
  Session live(f.header);
  live.append(std::vector<Event>(
      f.events.begin(),
      f.events.begin() + static_cast<std::ptrdiff_t>(f.late_prefix)));
  a::ProfileCurves curves;
  curves.update(live);
  auto before = curves.sample(request());
  check(before.samples[0].state == a::ProfileSampleState::incomplete &&
            before.provisional && !before.samples[0].value,
        "unflushed live capture presented final totals");
  auto subset = request();
  subset.partial = a::ProfilePartial::observed_subset;
  auto observed = curves.sample(subset);
  check(observed.samples[0].state == a::ProfileSampleState::incomplete &&
            observed.samples[0].exact_value == "0",
        "explicit provisional subset was not labeled");
  const auto old_count = curves.event_count();
  live.append(std::vector<Event>(f.events.begin() +
                                     static_cast<std::ptrdiff_t>(f.late_prefix),
                                 f.events.end()));
  curves.update(live);
  auto late = curves.sample(subset);
  check(late.samples[0].total_ticks == "40" &&
            late.samples[3].total_ticks == "350",
        "late cross-packet scope did not revise historical buckets");
  check(before.samples[0].total_ticks == "0",
        "new index append mutated returned series");
  rejects([&] { curves.update(live, old_count); });
  auto other = f.session();
  rejects([&] { curves.update(other); });
  live.finish();
  curves.update(live);
  check(!curves.sample(request()).provisional &&
            curves.sample(request()).samples[0].value,
        "sealed valid capture remained provisional");
  Fixture open;
  open.events.resize(open.late_prefix);
  auto unclosed = open.session();
  a::ProfileCurves unclosed_curves(unclosed);
  auto gap = unclosed_curves.sample(request());
  check(gap.samples[0].state == a::ProfileSampleState::incomplete &&
            !gap.samples[0].value,
        "unclosed scope was silently zero-filled");
  Fixture corrupt;
  corrupt.packet(Bytes{1, 15, 1, 0});
  auto damaged = corrupt.session();
  a::ProfileCurves broken(damaged);
  check(broken.sample(request()).samples[0].state ==
            a::ProfileSampleState::incomplete,
        "malformed packet did not invalidate completeness");
  auto permit = request();
  permit.partial = a::ProfilePartial::observed_subset;
  check(broken.sample(permit).samples[0].exact_value == "40",
        "observed subset disappeared after unrelated corruption");
  Fixture invalid;
  invalid.header.timestamp_denominator = 0;
  auto raw = invalid.session();
  a::ProfileCurves raw_curves(raw);
  auto ticks = raw_curves.sample(request());
  check(ticks.samples[0].value && !ticks.samples[0].calibration_valid,
        "invalid calibration erased exact native ticks");
  auto ns = request(a::ProfileMetric::total_nanoseconds);
  check(raw_curves.sample(ns).samples[0].state ==
                a::ProfileSampleState::invalid_clock &&
            !raw_curves.sample(ns).samples[0].value,
        "invalid clock fabricated nanoseconds");
  auto missing = request(a::ProfileMetric::count);
  missing.filter.event_id = 999;
  missing.coverage = a::ProfileCoverage::assume_continuous;
  check(curves.sample(missing).samples[0].state ==
            a::ProfileSampleState::not_collected,
        "unadvertised event was labeled zero matches");
  std::cout
      << "incremental late flush/provisional/integrity/invalid-clock passed\n";
}
void exact_and_generations() {
  Fixture f(ByteOrder::little, false);
  f.frame(0, UINT64_MAX);
  Wire raw;
  raw.span(0, UINT64_MAX - 1, 1, 7);
  raw.span(0, UINT64_MAX - 1, 1, 8);
  f.packet(raw.bytes);
  auto session = f.session();
  a::ProfileCurves curves(session);
  auto result = curves.sample(request());
  check(result.samples[0].total_ticks == "36893488147419103228" &&
            result.samples[0].exact_value == "36893488147419103228",
        "overlapping inclusive tick sum overflowed uint64");
  std::ostringstream csv;
  csv << std::hex << std::boolalpha;
  const auto flags = csv.flags();
  a::write_profile_curve_csv(csv, result);
  check(csv.str().find("36893488147419103228") != std::string::npos &&
            csv.str().find("18446744073709551615") != std::string::npos,
        "CSV converted wide sum or raw uint64 counter through double/hex");
  check(csv.flags() == flags, "CSV changed caller formatting");
  auto mean = request(a::ProfileMetric::mean_ticks);
  check(curves.sample(mean).samples[0].exact_value == "36893488147419103228/2",
        "mean did not preserve raw integer rational");
  Fixture generations;
  generations.emit(DestroyInstance{10});
  generations.announce();
  Wire next;
  next.span(201, 206);
  generations.packet(next.bytes);
  auto reused = generations.session();
  a::ProfileCurves lifetime(reused);
  auto q = request();
  q.filter.zone = pvd::profile::ZoneKey{10, 1};
  auto newer = lifetime.sample(q);
  check(newer.samples[1].completed_count == 1 &&
            newer.samples[1].total_ticks == "5",
        "zone generations merged");
  q.filter.zone = pvd::profile::ZoneKey{10, 0};
  check(lifetime.sample(q).samples[1].total_ticks == "190",
        "old zone lifetime altered after pointer reuse");
  Fixture reversed(ByteOrder::little, false);
  reversed.frame(300, 200);
  auto bad_frame = reversed.session();
  a::ProfileCurves reversed_curve(bad_frame);
  check(reversed_curve.sample(request()).samples[0].state ==
            a::ProfileSampleState::invalid_clock,
        "backwards native frame clock accepted");
  Fixture adjacent(ByteOrder::little, false);
  adjacent.frame(UINT64_MAX - 10, UINT64_MAX - 1);
  adjacent.frame(UINT64_MAX - 1, UINT64_MAX);
  auto almost = adjacent.session();
  a::ProfileCurves near_max(almost);
  auto close = near_max.sample(request());
  check(close.samples[1].elapsed_seconds &&
            *close.samples[1].elapsed_seconds == 5e-9,
        "adjacent high uint64 frame counters lost elapsed precision");
  std::cout << "wide exact CSV/mean/zone lifetime/high counters passed\n";
}

std::vector<std::string> split(std::string line) {
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  std::vector<std::string> values;
  std::size_t begin = 0;
  for (;;) {
    const auto at = line.find(',', begin);
    values.push_back(line.substr(begin, at - begin));
    if (at == std::string::npos)
      break;
    begin = at + 1;
  }
  return values;
}
void sdk(const char *capture, const char *expected_file,
         const char *windows_file, const char *scopes_file) {
  auto session = Session::load_file(capture);
  a::ProfileCurves curves(session);
  std::ifstream windows(windows_file), scopes(scopes_file),
      expected(expected_file);
  check(bool(windows) && bool(scopes) && bool(expected),
        "cannot open independent SDK frame oracle files");
  std::string line;
  std::uint64_t scene = 0, zone = 0, frequency = 0;
  std::size_t frame_count = 0;
  while (std::getline(windows, line)) {
    const auto c = split(line);
    if (c[0] == "frame_index")
      continue;
    const auto index = static_cast<std::size_t>(std::stoull(c.at(0)));
    scene = std::stoull(c.at(1));
    std::vector<const FrameMarker *> selected;
    for (const auto &frame : session.frames())
      if (frame.scene_id == scene)
        selected.push_back(&frame);
    check(index < selected.size(), "SDK native frame marker missing");
    const auto &f = *selected[index];
    check(f.begin_timestamp >= std::stoull(c.at(2)) &&
              f.begin_timestamp <= std::stoull(c.at(3)) &&
              f.end_timestamp >= std::stoull(c.at(4)) &&
              f.end_timestamp <= std::stoull(c.at(5)),
          "SDK native frame counter is outside independent QPC brackets");
    ++frame_count;
  }
  using Lane =
      std::tuple<std::uint64_t, std::uint16_t, std::uint32_t, std::uint64_t>;
  using Span = std::tuple<std::uint64_t, std::uint16_t, std::uint32_t,
                          std::uint64_t, std::uint64_t, std::uint64_t>;
  std::map<Lane, std::vector<std::uint64_t>> open;
  std::multiset<Span> oracle_spans;
  std::set<std::uint64_t> zone_ids;
  while (std::getline(scopes, line)) {
    const auto c = split(line);
    if (c[0] == "kind")
      continue;
    if (c[0] == "clock") {
      check(std::stoull(c.at(1)) == 1000000000, "oracle frequency numerator");
      frequency = std::stoull(c.at(2));
      continue;
    }
    zone = std::stoull(c.at(1));
    zone_ids.insert(zone);
    const auto event = static_cast<std::uint16_t>(std::stoul(c.at(2)));
    const auto thread = static_cast<std::uint32_t>(std::stoul(c.at(3)));
    const auto context = std::stoull(c.at(4)), ticks = std::stoull(c.at(7));
    const Lane key{zone, event, thread, context};
    if (c[0] == "start")
      open[key].push_back(ticks);
    else if (c[0] == "stop") {
      check(!open[key].empty(), "SDK parser oracle stop without start");
      const auto begin = open[key].back();
      open[key].pop_back();
      oracle_spans.emplace(zone, event, thread, context, begin, ticks);
    }
  }
  check(frame_count == 6 && oracle_spans.size() == 17 && frequency,
        "real SDK fixture coverage unexpectedly changed");
  check(session.header().timestamp_numerator == 1000000000 &&
            session.header().timestamp_denominator == frequency,
        "SDK profile/frame clock calibration mismatch");
  std::multiset<Span> actual_spans;
  std::size_t delayed = 0;
  const auto last_frame_prefix = session.frames().back().event_count;
  for (const auto &interval : curves.profile_index().intervals())
    if (zone_ids.count(interval.start_source.zone.id)) {
      actual_spans.emplace(interval.start_source.zone.id, interval.event_id,
                           interval.thread_id, interval.context_id,
                           interval.begin_ticks, interval.end_ticks);
      check(interval.duration_ticks ==
                interval.end_ticks - interval.begin_ticks,
            "raw SDK duration mismatch");
      delayed += interval.stop_source.event_index >= last_frame_prefix;
    }
  check(actual_spans == oracle_spans && delayed == 17,
        "decoded spans or deliberately delayed flush differ from independent "
        "SDK parser");
  std::size_t cases = 0;
  bool checked_long = false, checked_empty = false;
  while (std::getline(expected, line)) {
    const auto c = split(line);
    if (c[0] == "frame_index")
      continue;
    const auto frame = static_cast<std::size_t>(std::stoull(c.at(0)));
    scene = std::stoull(c.at(1));
    zone = std::stoull(c.at(2));
    auto q = request();
    q.source.scene_id = scene;
    q.filter.zone = pvd::profile::ZoneKey{zone, 0};
    const auto event = static_cast<std::uint16_t>(std::stoul(c.at(3)));
    const auto thread = static_cast<std::uint32_t>(std::stoul(c.at(4)));
    const auto context = std::stoull(c.at(5));
    if (event)
      q.filter.event_id = event;
    if (thread)
      q.filter.thread_id = thread;
    if (context)
      q.filter.context_id = context;
    q.coverage = a::ProfileCoverage::assume_continuous;
    const auto series = curves.sample(q);
    check(series.samples.size() == 6 && !series.provisional && series.complete,
          "real native profile curve is incomplete");
    const auto &value = series.samples[frame];
    const auto count = std::stoull(c.at(6)), total = std::stoull(c.at(7));
    check(value.completed_count == count && value.total_ticks == c.at(7),
          "profile curve differs from independent SDK per-frame count/total");
    check(value.state == (count ? a::ProfileSampleState::observed
                                : a::ProfileSampleState::no_matches),
          "real SDK empty/matched status wrong");
    if (count) {
      check(value.minimum_ticks == std::stoull(c.at(8)) &&
                value.maximum_ticks == std::stoull(c.at(9)),
            "real SDK min/max duration mismatch");
      q.metric = a::ProfileMetric::mean_ticks;
      const auto mean = curves.sample(q).samples[frame];
      check(mean.exact_value == c.at(7) + "/" + c.at(6),
            "real SDK mean rational changed");
      q.metric = a::ProfileMetric::maximum_nanoseconds;
      const auto ns = curves.sample(q).samples[frame];
      const auto expected_ns =
          static_cast<double>(static_cast<long double>(*value.maximum_ticks) *
                              1000000000 / frequency);
      check(ns.value && std::abs(a::plot_value(*ns.value) - expected_ns) <
                            std::max(1e-6, expected_ns * 1e-12),
            "real calibrated maximum duration mismatch");
    }
    if (event == 2 && frame == 2) {
      check(count == 1 && total > value.end_ticks - value.begin_ticks,
            "long scope did not preserve full duration across frame windows");
      checked_long = true;
    }
    if (event == 4 && frame == 4) {
      check(value.exact_value == "0",
            "explicitly collected no-match bucket not zero");
      checked_empty = true;
    }
    ++cases;
  }
  check(cases == 60 && checked_long && checked_empty,
        "real SDK frame oracle selectors missing");
  auto q = request();
  q.source.scene_id = scene;
  q.filter.zone = pvd::profile::ZoneKey{zone, 0};
  q.coverage = a::ProfileCoverage::assume_continuous;
  q.partial = a::ProfilePartial::observed_subset;
  Session live(session.header());
  live.append(
      std::vector<Event>(session.events().begin(),
                         session.events().begin() +
                             static_cast<std::ptrdiff_t>(last_frame_prefix)));
  a::ProfileCurves evolving(live);
  const auto before = evolving.sample(q);
  check(before.provisional && before.samples.size() == 6 &&
            before.samples[0].total_ticks == "0",
        "SDK delayed flush did not remain absent from received prefix");
  live.append(std::vector<Event>(
      session.events().begin() + static_cast<std::ptrdiff_t>(last_frame_prefix),
      session.events().end()));
  live.finish();
  evolving.update(live);
  const auto after = evolving.sample(q), complete = curves.sample(q);
  for (std::size_t i = 0; i < 6; ++i)
    check(after.samples[i].completed_count ==
                  complete.samples[i].completed_count &&
              after.samples[i].total_ticks == complete.samples[i].total_ticks,
          "real late SDK flush failed to revise historical frame windows");
  check(before.samples[0].total_ticks == "0" &&
            after.samples[0].completed_count > 0,
        "SDK historical result was mutated or delayed data was ignored");
  std::cout << "real SDK profile curves: " << frame_count
            << " native QPC-bracketed windows, " << actual_spans.size()
            << " parser-exact spans, " << cases
            << " selector buckets, late history revision passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 1) {
      buckets(ByteOrder::little);
      buckets(ByteOrder::big);
      stream_ordinals();
      incremental_and_quality();
      exact_and_generations();
    } else if (argc == 5)
      sdk(argv[1], argv[2], argv[3], argv[4]);
    else
      throw std::runtime_error("usage: profile_curves_tests [capture.pxd2 "
                               "expected.csv windows.csv scopes.csv]");
    std::cout << "profile curve tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "profile curve tests failed: " << e.what() << '\n';
    return 1;
  }
}
