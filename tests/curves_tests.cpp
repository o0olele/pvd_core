#include "pvd/curves.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
using namespace pvd;
namespace a = pvd::analysis;
namespace {
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
void near(double x, double y, const char *m) {
  check(std::isfinite(x) && std::abs(x - y) < 1e-9, m);
}
Bytes ints(std::initializer_list<std::uint64_t> values, std::size_t width,
           ByteOrder order) {
  Bytes out;
  for (auto n : values)
    for (std::size_t i = 0; i < width; ++i)
      out.push_back(static_cast<std::uint8_t>(
          n >> ((order == ByteOrder::little ? i : width - 1 - i) * 8)));
  return out;
}
Bytes floats(std::initializer_list<float> values, ByteOrder order) {
  Bytes out;
  for (auto f : values) {
    std::uint32_t n;
    std::memcpy(&n, &f, 4);
    auto bytes = ints({n}, 4, order);
    out.insert(out.end(), bytes.begin(), bytes.end());
  }
  return out;
}
struct Fixture {
  StreamHeader header;
  std::uint64_t stream = 1;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> handles;
  std::set<std::pair<std::uint64_t, std::uint32_t>> introduced;
  template <class T> void emit(T v) {
    events.push_back(
        {stream, events.size(), events.size() * 11, Payload{std::move(v)}});
  }
  std::uint32_t text(const std::string &s) {
    if (s.empty())
      return 0;
    auto found = handles.find(s);
    auto id = found == handles.end()
                  ? static_cast<std::uint32_t>(handles.size() + 1)
                  : found->second;
    if (found == handles.end())
      handles.emplace(s, id);
    if (introduced.insert({stream, id}).second)
      emit(StringHandleEvent{s, id});
    return id;
  }
  WireName type(const char *n, const char *ns = "physx3") {
    auto a = text(ns), b = text(n);
    return {a, b};
  }
  void prop(const char *n, const char *t, bool array = false,
            const char *ns = "physx3") {
    auto cls = type("Node", "test"), dt = type(t, ns);
    auto name = text(n);
    emit(CreateProperty{
        cls, name, 0, dt, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void value(const char *p, const char *t, Bytes data, std::uint32_t count = 1,
             const char *ns = "physx3") {
    auto name = text(p);
    auto dt = type(t, ns);
    emit(SetPropertyValue{100, name, dt, count, std::move(data)});
  }
  void u(const char *p, std::uint64_t n) {
    value(p, "PvdU64", ints({n}, 8, header.byte_order));
  }
  void pose(float y) {
    value("pose", "PxTransform",
          floats({0, 0, 0, 1, 1, y, 3}, header.byte_order));
  }
  void mark(bool begin, std::uint64_t ticks, const char *name = "frame",
            std::uint64_t scene = 1) {
    auto h = text(name);
    if (begin)
      emit(BeginSection{scene, h, ticks});
    else
      emit(EndSection{scene, h, ticks});
  }
  explicit Fixture(ByteOrder order = ByteOrder::little, bool ptr64 = true) {
    header.byte_order = order;
    header.stream_flags = ptr64 ? 1 : 0;
    header.timestamp_numerator = 1000;
    header.timestamp_denominator = 1;
    emit(CreateClass{type("Node", "test")});
    emit(CreateClass{type("Compound", "test")});
    emit(CreateClass{type("PxScene")});
    emit(CreateInstance{type("PxScene"), 1});
    emit(CreateInstance{type("Node", "test"), 100});
    emit(CreateInstance{type("Node", "test"), 200});
    for (const auto *p : {"u", "i", "late", "m\"ass,\nvalue"})
      prop(p, p == std::string("i") ? "PvdI64" : "PvdU64");
    prop("vec", "PxVec3");
    prop("quat", "PxQuat");
    prop("pose", "PxTransform");
    prop("arr", "PvdF32", true);
    prop("ref", "ObjectRef");
    prop("name", "StringHandle");
    prop("bool", "PvdBool");
    prop("bad_float", "PvdF32");
    prop("compound", "Compound", false, "test");
    prop("matrix", "PxMat33");
    prop("bounds", "PxBounds3");
    mark(true, 90);
    u("u", 9007199254740993ull);
    value("i", "PvdI64", ints({std::uint64_t(-9)}, 8, order));
    u("m\"ass,\nvalue", UINT64_MAX);
    value("vec", "PxVec3", floats({1, 2, 3}, order));
    value("quat", "PxQuat", floats({0, .5f, 0, 1}, order));
    pose(2);
    value("arr", "PvdF32", floats({1.5f, 2.5f}, order), 2);
    value("ref", "VoidPtr", ints({200}, header.pointer_width(), order));
    Bytes string(header.pointer_width(), 0);
    string.insert(string.end(), {'h', 'i', 0});
    value("name", "String", string);
    value("bool", "PvdBool", Bytes{1});
    value("bad_float", "PvdF32", floats({1}, order));
    value("compound", "Compound", Bytes{1, 2, 3}, 1, "test");
    value("matrix", "PxMat33", floats({1, 2, 3, 4, 5, 6, 7, 8, 9}, order));
    value("bounds", "PxBounds3", floats({1, 2, 3, 4, 5, 6}, order));
    mark(false, 100);
    mark(true, 101, "validation_after_fetch");
    pose(102);
    mark(false, 110, "validation_after_fetch");
    mark(true, 140, "frame", 2);
    mark(false, 150, "frame", 2);
    mark(true, 190);
    u("u", UINT64_MAX);
    value("i", "PvdI64", ints({std::uint64_t(1) << 63}, 8, order));
    u("late", 42);
    value("arr", "PvdF32", {}, 0);
    pose(4);
    value("vec", "PxVec3", floats({3, 4, 5}, order));
    value("bool", "PvdBool", Bytes{255});
    value("bad_float", "PvdF32",
          floats({std::numeric_limits<float>::quiet_NaN()}, order));
    value("ref", "VoidPtr", ints({0}, header.pointer_width(), order));
    emit(SetColor{100, {1, 2, 3, 4}});
    emit(SetPickable{100, false});
    emit(SetIsTopLevel{100, true});
    emit(SetCamera{"camera", {1, 2, 3}, {0, 1, 0}, {0, 0, 0}});
    emit(OriginShift{1, {1, 0, 0}});
    mark(false, 200);
    mark(true, 201, "validation_after_fetch");
    pose(104);
    mark(false, 210, "validation_after_fetch");
    mark(true, 290);
    emit(DestroyInstance{100});
    mark(false, 300);
    mark(true, 301, "validation_after_fetch");
    mark(false, 310, "validation_after_fetch");
    stream = 2;
    emit(CreateInstance{type("Node", "test"), 100});
    mark(true, 390);
    u("u", 50);
    pose(6);
    mark(false, 400);
    mark(true, 401, "validation_after_fetch");
    pose(106);
    mark(false, 410, "validation_after_fetch");
  }
  Session session() { return Session(header, events); }
};
a::CurveRequest request(const char *property, const char *component = "") {
  a::CurveRequest q;
  q.source.scene_id = 1;
  q.object = {100, 0};
  q.property = property;
  q.component = component;
  return q;
}
const a::ObjectChange *change(const a::StateDiff &d, std::uint64_t id,
                              a::ChangeKind kind) {
  for (const auto &c : d.objects)
    if (c.object.id == id && c.kind == kind)
      return &c;
  return nullptr;
}
const a::PropertyChange *property(const a::ObjectChange &c, const char *name) {
  for (const auto &p : c.properties)
    if (p.name == name)
      return &p;
  return nullptr;
}
void numeric_curves_and_gaps() {
  Fixture f;
  auto s = f.session();
  auto u = a::sample_curve(s, request("u"));
  check(u.complete && u.samples.size() == 4 && u.candidate_samples == 4,
        "native scene sample count");
  check(std::get<std::uint64_t>(*u.samples[0].value) == 9007199254740993ull &&
            std::get<std::uint64_t>(*u.samples[1].value) == UINT64_MAX,
        "uint64 values retain exact precision");
  check(u.samples[2].gap == a::Gap::object_absent &&
            u.samples[3].gap == a::Gap::generation_mismatch,
        "lifecycle gaps do not reuse later object IDs");
  check(u.samples[0].source.timestamp_ticks == 100 &&
            u.samples[3].source.timestamp_ticks == 400,
        "raw native ticks");
  near(*u.samples[0].seconds, .0001, "header calibrated seconds");
  near(*u.samples[3].elapsed_seconds, .0003, "relative elapsed seconds");
  auto i = a::sample_curve(s, request("i"));
  check(std::get<std::int64_t>(*i.samples[1].value) == INT64_MIN,
        "signed int64 minimum");
  auto vec = a::sample_curve(s, request("vec", "y"));
  near(a::plot_value(*vec.samples[0].value), 2, "vec component");
  auto q = a::sample_curve(s, request("quat", "w"));
  near(a::plot_value(*q.samples[0].value), 1, "quaternion component");
  auto pose = a::sample_curve(s, request("pose", "Transform.position.y"));
  near(a::plot_value(*pose.samples[0].value), 2,
       "transform translation offset");
  auto post = request("pose", "position.y");
  post.source.sampling = a::Sampling::after_fetch;
  auto after = a::sample_curve(s, post);
  near(a::plot_value(*after.samples[0].value), 102,
       "native marker is not post-fetch");
  check(after.samples[0].source.timestamp_ticks == 110,
        "explicit post-fetch ticks");
  auto arr = request("arr");
  arr.item_index = 1;
  auto array = a::sample_curve(s, arr);
  near(a::plot_value(*array.samples[0].value), 2.5, "array item selection");
  check(array.samples[1].gap == a::Gap::item_missing, "empty array is a gap");
  auto late = a::sample_curve(s, request("late"));
  check(late.samples[0].gap == a::Gap::property_missing &&
            a::plot_value(*late.samples[1].value) == 42,
        "uncollected property is not zero");
  for (const auto *name : {"ref", "name"})
    check(a::sample_curve(s, request(name)).samples[0].gap ==
              a::Gap::not_numeric,
          "pointer or string handle must not be graphed as a number");
  check(a::sample_curve(s, request("compound")).samples[0].gap ==
            a::Gap::unsupported_type,
        "opaque custom values produce explicit gap");
  check(a::sample_curve(s, request("vec", "w")).samples[0].gap ==
            a::Gap::invalid_component,
        "invalid numeric component gap");
  check(a::sample_curve(s, request("bad_float")).samples[1].gap ==
            a::Gap::non_finite,
        "non-finite value gap");
  check(a::sample_curve(s, request("bool")).samples[1].gap ==
            a::Gap::malformed_value,
        "invalid boolean raw value gap");
  near(a::plot_value(
           *a::sample_curve(s, request("matrix", "m[1][2]")).samples[0].value),
       8, "matrix is column major");
  near(
      a::plot_value(
          *a::sample_curve(s, request("bounds", "maximum.z")).samples[0].value),
      6, "bounds maximum component");
  auto next = request("u");
  next.object.generation = 1;
  auto reused = a::sample_curve(s, next);
  check(reused.samples[0].gap == a::Gap::generation_mismatch &&
            std::get<std::uint64_t>(*reused.samples[3].value) == 50,
        "new generation sampled independently");
  auto stream = request("u");
  stream.source.stream_id = 2;
  auto filtered = a::sample_curve(s, stream);
  check(filtered.samples.size() == 1 &&
            filtered.samples[0].source.frame_index == 3,
        "stream filter preserves scene timeline ordinal");
  check(!a::locate_frame(s, {stream.source, 0}) &&
            a::locate_frame(s, {stream.source, 3})->timestamp_ticks == 400,
        "frame lookup matches viewer timeline");
  auto stepped = request("u");
  stepped.stride = 2;
  auto stride = a::sample_curve(s, stepped);
  check(stride.samples.size() == 2 && stride.samples[1].break_before,
        "stride does not fabricate observed continuity");
  a::CurveOptions options;
  options.max_samples = 3;
  auto decimated = a::sample_curve(s, request("u"), options);
  check(decimated.resampled && decimated.samples.size() == 3 &&
            decimated.samples.front().source.frame_index == 0 &&
            decimated.samples.back().source.frame_index == 3 &&
            decimated.samples.back().break_before,
        "uniform decimation retains endpoints and break markers");
  options.resampling = a::Resampling::reject;
  bool rejected = false;
  try {
    a::sample_curve(s, request("u"), options);
  } catch (const std::length_error &) {
    rejected = true;
  }
  check(rejected, "sample limit reject policy");
  bool stop = false;
  options = {};
  options.cancelled = [&] { return stop; };
  options.progress = [&](std::size_t n, std::size_t total) {
    check(total == 4, "progress selected row count");
    if (n == 2)
      stop = true;
  };
  auto cancelled = a::sample_curve(s, request("u"), options);
  check(cancelled.cancelled && !cancelled.complete &&
            cancelled.samples.size() == 2,
        "cancel returns explicit valid partial series");
  std::ostringstream csv;
  a::write_curve_csv(csv, u);
  auto text = csv.str();
  check(text.find("9007199254740993") != std::string::npos &&
            text.find("18446744073709551615") != std::string::npos,
        "CSV uint64 never goes through double");
  std::ostringstream signed_csv;
  a::write_curve_csv(signed_csv, i);
  check(signed_csv.str().find("-9223372036854775808") != std::string::npos,
        "CSV int64 retains minimum");
  std::ostringstream escaped;
  a::write_curve_csv(escaped, a::sample_curve(s, request("m\"ass,\nvalue")));
  check(escaped.str().find("\"m\"\"ass,\nvalue\"") != std::string::npos,
        "RFC4180 quote/comma/newline names");
}
void endian_clock_and_limits() {
  Fixture f(ByteOrder::big, false);
  auto s = f.session();
  auto curve = a::sample_curve(s, request("u"));
  check(std::get<std::uint64_t>(*curve.samples[1].value) == UINT64_MAX,
        "big endian uint64");
  auto signed_value = a::sample_curve(s, request("i"));
  check(std::get<std::int64_t>(*signed_value.samples[1].value) == INT64_MIN,
        "big endian signed64");
  auto transform = a::sample_curve(s, request("pose", "position.y"));
  near(a::plot_value(*transform.samples[1].value), 4, "big endian transform");
  check(a::sample_curve(s, request("ref")).samples[0].gap ==
            a::Gap::not_numeric,
        "32-bit source pointers are not numeric");
  // Relative timing must preserve a one-tick difference near uint64 maximum,
  // including MSVC where long double has only double precision.
  Fixture large;
  std::size_t changed_ticks = 0;
  for (auto &event : large.events)
    if (auto end = std::get_if<EndSection>(&event.payload))
      if (end->section_id == 1 && end->name == large.handles.at("frame") &&
          changed_ticks < 2)
        end->timestamp = UINT64_MAX - 1000 + changed_ticks++;
  auto large_session = large.session();
  auto large_curve = a::sample_curve(large_session, request("u"));
  near(*large_curve.samples[1].elapsed_seconds, .000001,
       "exact uint64 tick delta before conversion");
  std::ostringstream large_csv;
  a::write_curve_csv(large_csv, large_curve);
  check(large_csv.str().find(std::to_string(UINT64_MAX - 1000)) !=
            std::string::npos,
        "CSV raw ticks preserve uint64 precision");
  f.header.timestamp_numerator = 0;
  auto no_clock = f.session();
  auto ticks = a::sample_curve(no_clock, request("u"));
  check(!ticks.samples[0].seconds && !ticks.samples[0].elapsed_seconds &&
            !ticks.diagnostics.empty() &&
            ticks.samples[0].source.timestamp_ticks == 100,
        "missing calibration retains ticks only");
  auto source = request("u");
  source.first_frame = 1;
  source.end_frame = 2;
  auto range = a::sample_curve(s, source);
  check(range.samples.size() == 1 && range.samples[0].source.frame_index == 1,
        "half-open frame range");
  source.stride = 0;
  bool rejected = false;
  try {
    a::sample_curve(s, source);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  check(rejected, "zero stride rejected");
  a::CurveOptions stop;
  stop.cancelled = [] { return true; };
  auto cancelled = a::sample_curve(s, request("u"), stop);
  check(cancelled.cancelled && cancelled.samples.empty(),
        "cancel before enumeration");
}
void state_diff() {
  Fixture f;
  auto s = f.session();
  a::FrameSource source;
  source.scene_id = 1;
  auto first = *a::locate_frame(s, {source, 0}),
       second = *a::locate_frame(s, {source, 1});
  auto before = s.snapshot_at_event(first.event_count),
       after = s.snapshot_at_event(second.event_count);
  auto diff = a::diff_snapshots(before, after);
  auto object = change(diff, 100, a::ChangeKind::changed);
  check(diff.complete && object && object->pickable_changed &&
            object->top_level_changed && object->color_changed,
        "diff display flags and color");
  check(property(*object, "ref")->references_changed &&
            property(*object, "u")->data_changed &&
            property(*object, "late")->before == std::nullopt,
        "diff references, payload and added property");
  check(diff.cameras.size() == 1 && diff.origin_shifts.size() == 1,
        "camera and origin state changes");
  auto events = a::diff_events(s, first.event_count, second.event_count);
  check(events.objects.size() == diff.objects.size() &&
            events.before_event_count == first.event_count,
        "event prefix diff");
  auto frames = a::diff_frames(s, {source, 0}, {source, 3});
  auto destroyed = change(frames, 100, a::ChangeKind::destroyed),
       created = change(frames, 100, a::ChangeKind::created);
  check(destroyed && created && destroyed->object.generation == 0 &&
            created->object.generation == 1,
        "ID reuse produces separate old/new lifetimes");
  auto reverse = a::diff_frames(s, {source, 3}, {source, 0});
  check(
      change(reverse, 100, a::ChangeKind::destroyed)->object.generation == 1 &&
          change(reverse, 100, a::ChangeKind::created)->object.generation == 0,
      "reverse diff lifecycle direction");
  auto post = source;
  post.sampling = a::Sampling::after_fetch;
  auto cross = a::diff_frames(s, {source, 0}, {post, 0});
  check(change(cross, 100, a::ChangeKind::changed)->properties.size() == 1 &&
            property(*change(cross, 100, a::ChangeKind::changed), "pose"),
        "diff native versus explicit post-fetch");
  auto nan_camera = before;
  nan_camera.cameras["nan"] = {
      {std::numeric_limits<float>::quiet_NaN(), 0, 0}, {0, 1, 0}, {}};
  auto identical_nan = nan_camera;
  check(a::diff_snapshots(nan_camera, identical_nan).cameras.empty(),
        "identical NaN camera bits do not create false differences");
  auto copy = before;
  copy.event_count = 999;
  copy.instances.at(100).properties.at("u").bytes =
      std::make_shared<const Bytes>(
          before.instances.at(100).property("u")->raw_bytes());
  check(a::diff_snapshots(before, copy).objects.empty(),
        "equal bytes in distinct immutable buffers are unchanged");
  copy = before;
  copy.instances.at(100).properties.at("name").text = "changed decoded text";
  copy.instances.at(100).properties.at("ref").reference_ids =
      std::vector<std::uint64_t>{999};
  auto derived = a::diff_snapshots(before, copy);
  auto c = change(derived, 100, a::ChangeKind::changed);
  check(property(*c, "name")->text_changed &&
            !property(*c, "name")->data_changed &&
            property(*c, "ref")->references_changed &&
            !property(*c, "ref")->data_changed,
        "text and normalized refs diff independently of raw bytes");
  a::DiffOptions limits;
  limits.max_objects = 1;
  auto objects = a::diff_snapshots(before, after, limits);
  check(objects.truncated && !objects.complete, "visited-object diff budget");
  limits = {};
  limits.max_properties = 1;
  auto properties = a::diff_snapshots(before, after, limits);
  check(properties.truncated && !properties.complete,
        "visited-property diff budget");
  limits = {};
  limits.max_retained_value_bytes = 1;
  auto retained = a::diff_snapshots(before, after, limits);
  check(retained.truncated && !retained.complete, "retained-value diff budget");
  limits = {};
  limits.cancelled = [] { return true; };
  auto cancelled = a::diff_snapshots({}, {}, limits);
  check(cancelled.cancelled && !cancelled.complete,
        "empty diff is cancellable");
}
void real_curve(const char *path) {
  auto session = Session::load_file(path);
  check(!session.frames().empty(), "real curve needs frames");
  auto marker = session.frames().front();
  auto snapshot = session.snapshot_at_event(marker.event_count);
  std::uint64_t object = 0, generation = 0;
  for (const auto &[id, state] : snapshot.instances)
    if (state.class_name.name == "PxRigidDynamic" &&
        state.property("GlobalPose")) {
      object = id;
      generation = state.generation;
      break;
    }
  check(object != 0, "real dynamic actor");
  auto q = request("GlobalPose", "position.y");
  q.object = {object, generation};
  q.source.scene_id = marker.scene_id;
  auto series = a::sample_curve(session, q);
  check(series.samples.size() >= 12 && series.samples.front().value &&
            series.samples.back().value,
        "real SDK transform curve");
  for (const auto &sample : series.samples) {
    auto state = session.snapshot_at_event(sample.source.event_count);
    auto o = state.instance(object);
    if (sample.value) {
      check(o && o->generation == generation, "real curve lifetime");
      near(a::plot_value(*sample.value),
           o->property("GlobalPose")->as_transform()->position.y,
           "real typed property curve matches core snapshot");
    }
  }
  std::ostringstream csv;
  a::write_curve_csv(csv, series);
  check(csv.str().find("GlobalPose") != std::string::npos, "real curve CSV");
  auto diff = a::diff_events(session, series.samples.front().source.event_count,
                             series.samples.back().source.event_count);
  check(diff.complete && change(diff, object, a::ChangeKind::changed),
        "real frame property diff");
}
void origin_coordinates() {
  Fixture f;
  auto rigid=f.type("PxRigidStatic"),transform=f.type("PxTransform"),
       refs=f.type("ObjectRef"),pointer=f.type("VoidPtr");
  f.emit(CreateClass{rigid});
  const auto global=f.text("GlobalPose"),scene=f.text("Scene");
  f.emit(CreateProperty{rigid,global,0,transform,1,{}});
  f.emit(CreateProperty{rigid,scene,0,refs,1,{}});
  f.emit(CreateInstance{rigid,500});
  f.emit(SetPropertyValue{500,scene,pointer,1,ints({1},f.header.pointer_width(),f.header.byte_order)});
  f.emit(SetPropertyValue{500,global,transform,1,floats({0,0,0,1,100,20,30},f.header.byte_order)});
  f.mark(true,500,"origin_test");f.mark(false,501,"origin_test");
  const auto before=f.events.size();
  f.emit(OriginShift{1,{10,2,-3}});
  f.mark(true,510,"origin_test");f.mark(false,511,"origin_test");
  Session s(f.header,f.events);
  a::CurveRequest q;q.source.scene_id=1;q.source.sampling=a::Sampling::after_fetch;
  q.source.after_fetch_section="origin_test";q.object={500,0};q.property="GlobalPose";q.component="position.x";
  auto native=a::sample_curve(s,q);check(native.samples.size()==2,"origin curve count");
  near(a::plot_value(*native.samples[0].value),100,"native origin baseline");
  near(a::plot_value(*native.samples[1].value),90,"native origin movement absent");
  q.coordinate_space=a::CoordinateSpace::wire;auto wire=a::sample_curve(s,q);
  near(a::plot_value(*wire.samples[1].value),100,"wire curve must retain last payload");
  std::ostringstream csv;a::write_curve_csv(csv,wire);
  check(csv.str().find("coordinate_space")!=std::string::npos&&csv.str().find(",wire\r\n")!=std::string::npos,"CSV coordinate semantics missing");
  auto native_diff=a::diff_events(s,before,f.events.size());
  auto changed=std::find_if(native_diff.objects.begin(),native_diff.objects.end(),[](const auto&o){return o.object.id==500;});
  check(changed!=native_diff.objects.end()&&changed->properties.size()==1&&changed->properties[0].coordinates_changed&&!changed->properties[0].wire_data_changed,"native diff raw/coordinate distinction");
  a::DiffOptions opts;opts.coordinate_space=a::CoordinateSpace::wire;
  auto raw_diff=a::diff_events(s,before,f.events.size(),opts);
  check(std::none_of(raw_diff.objects.begin(),raw_diff.objects.end(),[](const auto&o){return o.object.id==500;}),"wire diff invents static setter");
  opts={};opts.max_retained_value_bytes=56;
  check(a::diff_events(s,before,f.events.size(),opts).truncated,"native shadow omitted from retained-value budget");
}
} // namespace
int main(int argc, char **argv) {
  try {
    numeric_curves_and_gaps();
    endian_clock_and_limits();
    state_diff();
    origin_coordinates();
    if (argc > 1)
      real_curve(argv[1]);
    std::cout << "curves tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
