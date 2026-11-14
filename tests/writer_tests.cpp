#include "pvd/writer.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace io = pvd::writing;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F &&f) {
  try {
    f();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("expected rejection");
}
Bytes bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }
void u(Bytes &b, std::uint64_t value, std::size_t width, ByteOrder order) {
  for (std::size_t n = 0; n < width; ++n)
    b.push_back(static_cast<std::uint8_t>(
        value >> ((order == ByteOrder::little ? n : width - n - 1) * 8)));
}
Bytes number(std::uint64_t value, std::size_t width, ByteOrder order) {
  Bytes out;
  u(out, value, width, order);
  return out;
}
std::vector<Event> all_events() {
  constexpr std::uint64_t id = 0xfedcba9876543210ull;
  std::vector<Payload> p;
  p.emplace_back(StringHandleEvent{u8"资源🚀", 0xffffffffu});
  p.emplace_back(CreateClass{{11, 12}});
  p.emplace_back(DeriveClass{{1, 2}, {3, 4}});
  p.emplace_back(CreateProperty{
      {11, 12}, 20, 21, {22, 23}, 2, {{24, 0xffffffffu}, {25, 123}}});
  p.emplace_back(CreatePropertyMessage{
      {11, 12}, {30, 31}, {{32, {33, 34}, 4, 8}, {35, {36, 37}, 12, 16}}, 28});
  p.emplace_back(CreateInstance{{11, 12}, id});
  p.emplace_back(
      SetPropertyValue{id, 40, {41, 42}, 5, {0x12, 0x34, 0x56, 0x78}});
  p.emplace_back(BeginSetPropertyValue{id, 43, {44, 45}});
  p.emplace_back(AppendPropertyValueData{{1, 2, 3, 0, 255}, 6});
  p.emplace_back(EndSetPropertyValue{});
  p.emplace_back(SetPropertyMessage{id, {46, 47}, {0, 4, 6, 0xff}});
  p.emplace_back(BeginPropertyMessageGroup{{48, 49}});
  p.emplace_back(SendPropertyMessageFromGroup{id, {1, 0, 0xff}});
  p.emplace_back(EndPropertyMessageGroup{});
  p.emplace_back(DestroyInstance{id});
  p.emplace_back(PushBackObjectRef{id, 50, 0xffffffffffffffffull});
  p.emplace_back(RemoveObjectRef{id, 52, 53});
  p.emplace_back(BeginSection{54, 55, 0xffffffffffffffffull});
  p.emplace_back(EndSection{57, 58, 59});
  p.emplace_back(SetPickable{id, true});
  p.emplace_back(SetColor{id, {60, 61, 62, 63}});
  p.emplace_back(SetIsTopLevel{id, false});
  p.emplace_back(
      SetCamera{"camera", {1.25f, -2.5f, 3.75f}, {0, 1, 0}, {-1, -2, -3}});
  p.emplace_back(AddProfileZone{id, "zone"});
  p.emplace_back(AddProfileZoneEvent{id, "simulate", 0xabcd, true});
  p.emplace_back(StreamEndEvent{""});
  p.emplace_back(ErrorMessage{64, "message", "file.cpp", 65});
  p.emplace_back(OriginShift{id, {100, -200, 300}});
  std::vector<Event> result;
  for (auto &v : p)
    result.push_back(
        {0x1122334455667788ull, 0xffeeddccbbaa9988ull, 0, std::move(v)});
  return result;
}
void same_event(const Event &a, const Event &b, ByteOrder order) {
  check(a.stream_id == b.stream_id && a.timestamp == b.timestamp &&
            a.tag() == b.tag(),
        "roundtrip event group fields");
  check(io::encode_event(a, order) == io::encode_event(b, order),
        "roundtrip payload fields and opaque bytes");
}
void same_value(const PropertyValue &a, const PropertyValue &b) {
  check(a.incoming_type == b.incoming_type && a.item_count == b.item_count &&
            a.byte_order == b.byte_order &&
            a.pointer_width == b.pointer_width &&
            a.raw_bytes() == b.raw_bytes() && a.text == b.text &&
            a.reference_ids == b.reference_ids && a.opaque == b.opaque,
        "snapshot value exact encoding and resolved state");
}
bool same_vec(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
void same_snapshot(const Snapshot &a, const Snapshot &b) {
  check(a.instances.size() == b.instances.size() &&
            a.cameras.size() == b.cameras.size() &&
            a.origin_shifts.size() == b.origin_shifts.size(),
        "snapshot collection sizes");
  for (const auto &item : a.instances) {
    const auto *rhs = b.instance(item.first);
    const auto &lhs = item.second;
    check(rhs && lhs.generation == rhs->generation &&
              lhs.creating_stream == rhs->creating_stream &&
              lhs.class_name == rhs->class_name &&
              lhs.pickable == rhs->pickable &&
              lhs.top_level == rhs->top_level &&
              lhs.properties.size() == rhs->properties.size() &&
              lhs.color.has_value() == rhs->color.has_value(),
          "snapshot instance identity flags and generation");
    if (lhs.color)
      check(std::memcmp(&*lhs.color, &*rhs->color, sizeof(Color)) == 0,
            "snapshot color");
    for (const auto &prop : lhs.properties) {
      auto value = rhs->property(prop.first);
      check(value != nullptr, "snapshot property exists");
      same_value(prop.second, *value);
    }
  }
  for (const auto &camera : a.cameras) {
    const auto &rhs = b.cameras.at(camera.first);
    check(same_vec(camera.second.position, rhs.position) &&
              same_vec(camera.second.up, rhs.up) &&
              same_vec(camera.second.target, rhs.target),
          "snapshot cameras");
  }
  for (const auto &shift : a.origin_shifts)
    check(same_vec(shift.second, b.origin_shifts.at(shift.first)),
          "snapshot origins");
}
Session roundtrip(const Session &source, io::WriteOptions options = {}) {
  std::ostringstream out(std::ios::binary);
  auto result = io::write_session(out, source, options);
  check(result.state == io::WriteState::completed &&
            result.event_count == source.event_count() &&
            !result.discarded_tail_events &&
            result.bytes_written == out.str().size(),
        "Session write result");
  auto decoded = Session::parse(bytes(out.str()));
  check(decoded.event_count() == source.event_count(),
        "Session roundtrip event count");
  for (std::size_t i = 0; i < source.event_count(); ++i)
    same_event(source.events()[i], decoded.events()[i],
               source.header().byte_order);
  same_snapshot(source.latest(), decoded.latest());
  check(source.frames().size() == decoded.frames().size() &&
            source.sections().size() == decoded.sections().size() &&
            source.lifetimes().size() == decoded.lifetimes().size() &&
            source.channels().size() == decoded.channels().size() &&
            source.metadata().classes.size() ==
                decoded.metadata().classes.size() &&
            source.metadata().messages.size() ==
                decoded.metadata().messages.size() &&
            source.profile_zones().size() == decoded.profile_zones().size(),
        "all state index counts preserved");
  for (std::size_t i = 0; i < source.frames().size(); ++i) {
    const auto &a = source.frames()[i];
    const auto &b = decoded.frames()[i];
    check(a.stream_id == b.stream_id && a.scene_id == b.scene_id &&
              a.index == b.index &&
              a.begin_event_count == b.begin_event_count &&
              a.event_count == b.event_count &&
              a.begin_timestamp == b.begin_timestamp &&
              a.end_timestamp == b.end_timestamp,
          "frame index exact");
    same_snapshot(source.snapshot_at_frame(i), decoded.snapshot_at_frame(i));
  }
  for (std::size_t i = 0; i < source.channels().size(); ++i) {
    const auto &a = source.channels()[i];
    const auto &b = decoded.channels()[i];
    check(a.event_index == b.event_index && a.stream_id == b.stream_id &&
              a.instance_id == b.instance_id &&
              a.property_name == b.property_name && a.semantic == b.semantic,
          "channel source exact");
    same_value(a.value, b.value);
  }
  return decoded;
}
void raw() {
  for (const auto order : {ByteOrder::little, ByteOrder::big}) {
    StreamHeader h;
    h.byte_order = order;
    h.stream_flags = 1;
    h.timestamp_numerator = 0xfedcba9876543210ull;
    h.timestamp_denominator = 0x8877665544332211ull;
    auto events = all_events();
    std::ostringstream out(std::ios::binary);
    auto result = io::write_raw(out, h, events);
    StreamDecoder d;
    d.feed(bytes(out.str()));
    d.finish();
    check(d.events().size() == 28 && d.group_count() == 1 &&
              d.header()->timestamp_numerator == h.timestamp_numerator &&
              d.header()->timestamp_denominator == h.timestamp_denominator,
          "all28 and exact u64 header");
    for (std::size_t i = 0; i < events.size(); ++i)
      same_event(events[i], d.events()[i], order);
    check(result.event_count == 28 && result.group_count == 1,
          "single group result");
    events[3].stream_id = 1;
    events[8].timestamp = 2;
    io::WriteOptions options;
    options.target_group_bytes = 60;
    options.max_group_bytes = 256;
    options.max_events_per_group = 2;
    std::ostringstream repacked;
    auto many = io::write_raw(repacked, h, events, options);
    StreamDecoder rd;
    rd.feed(bytes(repacked.str()));
    rd.finish();
    check(many.group_count > 14 && rd.events().size() == events.size(),
          "groups repacked with budgets and context switches");
    for (std::size_t i = 0; i < events.size(); ++i)
      same_event(events[i], rd.events()[i], order);
    h.timestamp_denominator = 0;
    rejects([&] { io::encode_header(h); });
  }
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::uint64_t, std::map<std::string, std::uint32_t>> handles;
  explicit Fixture(ByteOrder order) {
    header.byte_order = order;
    header.stream_flags = 1;
    header.timestamp_numerator = 100;
    header.timestamp_denominator = 3;
  }
  template <class T> void emit(T p, std::uint64_t stream = 1) {
    events.push_back({stream, events.size() * 31, 0, Payload{std::move(p)}});
  }
  std::uint32_t str(const std::string &text, std::uint64_t stream = 1) {
    if (text.empty())
      return 0;
    auto &hs = handles[stream];
    auto found = hs.find(text);
    if (found != hs.end())
      return found->second;
    auto h = static_cast<std::uint32_t>(hs.size() + 1);
    hs.emplace(text, h);
    emit(StringHandleEvent{text, h}, stream);
    return h;
  }
  WireName name(const char *n, const char *ns = "physx3",
                std::uint64_t stream = 1) {
    auto a = str(ns, stream), b = str(n, stream);
    return {a, b};
  }
  void property(const char *n, const char *type, bool array = false,
                const char *semantic = "") {
    auto cls = name("Node", "test"), dt = name(type);
    auto prop = str(n), sem = str(semantic);
    emit(CreateProperty{
        cls, prop, sem, dt, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void set(std::uint64_t id, const char *prop, const char *type, Bytes data,
           std::uint32_t count = 1, std::uint64_t stream = 1) {
    auto p = str(prop, stream);
    auto t = name(type, "physx3", stream);
    emit(SetPropertyValue{id, p, t, count, std::move(data)}, stream);
  }
  void setup() {
    auto cls = name("Node", "test");
    emit(CreateClass{cls});
    property("Value", "PvdU64");
    property("Name", "String");
    property("Label", "StringHandle");
    property("Children", "ObjectRef", true);
    property("Data", "PvdU8", true);
    property("Events", "PvdU8", true, "render event stream");
    auto derived = name("Derived", "test");
    emit(CreateClass{derived});
    emit(DeriveClass{cls, derived});
    emit(CreateInstance{derived, 0xfedcba9876543210ull});
    emit(CreateInstance{cls, 99});
    set(99, "Value", "PvdU64", number(UINT64_MAX, 8, header.byte_order));
    Bytes text(8, 0xff);
    text.insert(text.end(), {'h', 'e', 'l', 'l', 'o', 0});
    set(99, "Name", "String", text);
    auto label = str("Label with comma, and UTF8", 2);
    set(99, "Label", "StringHandle", number(label, 4, header.byte_order), 1, 2);
    set(99, "Data", "PvdU8", Bytes(1024u * 1024u + 7, 0xaa), 1024u * 1024u + 7);
    set(99, "Events", "PvdU8", {0, 1, 2, 3}, 4);
    auto children = str("Children");
    emit(PushBackObjectRef{0xfedcba9876543210ull, children, 99});
    emit(SetPickable{99, false});
    emit(SetIsTopLevel{99, true});
    emit(SetColor{99, {23, 45, 67, 89}});
    emit(SetCamera{"Camera", {1, 2, 3}, {0, 1, 0}, {0, 0, 0}});
    emit(OriginShift{99, {1.5f, 2.5f, 3.5f}});
    auto value = str("Value");
    auto msg = name("ValueMessage", "test");
    auto u64 = name("PvdU64");
    emit(CreatePropertyMessage{cls, msg, {{value, u64, 0, 8}}, 8});
  }
  void frame(unsigned n) {
    const auto marker = str("frame");
    emit(BeginSection{99, marker, n * 100});
    auto msg = name("ValueMessage", "test");
    emit(BeginPropertyMessageGroup{msg});
    emit(SendPropertyMessageFromGroup{99, number(n, 8, header.byte_order)});
    emit(EndPropertyMessageGroup{});
    auto data = str("Data");
    auto u8 = name("PvdU8");
    emit(BeginSetPropertyValue{99, data, u8});
    emit(AppendPropertyValueData{{1, 2, 3}, 3});
    emit(AppendPropertyValueData{{4, 5}, 2});
    emit(EndSetPropertyValue{});
    emit(EndSection{99, marker, n * 100 + 50});
  }
};
void semantic(ByteOrder order) {
  Fixture f(order);
  f.setup();
  const auto large_begin = f.events.size();
  f.frame(0);
  for (unsigned n = 0; n < 100; ++n)
    f.set(99, "Value", "PvdU64", number(n, 8, order));
  auto before = f.events.size();
  f.frame(1);
  auto after = f.events.size();
  f.emit(DestroyInstance{99});
  f.emit(CreateInstance{f.name("Node", "test"), 99});
  f.set(99, "Value", "PvdU64", number(123456789123456789ull, 8, order));
  f.frame(2);
  Session source(f.header, f.events);
  auto decoded = roundtrip(source);
  (void)decoded;
  auto large =
      io::crop_events(source, large_begin, source.frames()[0].event_count);
  Session large_session(large.header, large.events);
  same_snapshot(source.snapshot_at_event(large_begin),
                large_session.snapshot_at_event(large.bootstrap_event_count));
  roundtrip(large_session);
  auto crop = io::crop_events(source, before, after);
  Session cropped(crop.header, crop.events);
  check(crop.bootstrap_event_count > 0 &&
            crop.events.size() < source.event_count(),
        "crop replaces old motion with bootstrap");
  same_snapshot(source.snapshot_at_event(before),
                cropped.snapshot_at_event(crop.bootstrap_event_count));
  same_snapshot(source.snapshot_at_event(after), cropped.latest());
  for (std::size_t i = 0; i < after - before; ++i)
    same_event(source.events()[before + i],
               cropped.events()[crop.bootstrap_event_count + i], order);
  check(cropped.frames().size() == 1 &&
            cropped.frames()[0].begin_timestamp == 100,
        "crop frame boundaries preserved");
  roundtrip(cropped);
  auto framed = io::crop_frames(source, 1, 2);
  Session framed_session(framed.header, framed.events);
  same_snapshot(source.snapshot_at_event(source.frames()[2].event_count),
                framed_session.latest());
  check(framed_session.latest().instance(99)->generation == 1,
        "selected ID reuse generation retained");
  auto final = io::crop_events(source, source.frames()[2].event_count,
                               source.event_count());
  Session reused(final.header, final.events);
  same_snapshot(source.latest(),
                reused.snapshot_at_event(final.bootstrap_event_count));
  check(reused.latest().instance(99)->generation == 1,
        "pre-crop ID reuse generation retained");
  std::size_t chunk = 0;
  for (std::size_t i = 0; i < f.events.size(); ++i)
    if (std::holds_alternative<BeginSetPropertyValue>(f.events[i].payload)) {
      chunk = i;
      break;
    }
  check(chunk > 0, "fixture chunk exists");
  rejects([&] { io::crop_events(source, chunk + 1, after); });
  Session live(f.header);
  live.append(std::vector<Event>(f.events.begin(),
                                 f.events.begin() +
                                     static_cast<std::ptrdiff_t>(chunk + 2)));
  const auto prefix = io::safe_prefix(live);
  check(prefix.event_count == chunk && prefix.discarded_tail_events == 2,
        "safe prefix excludes pending global transaction");
  std::ostringstream partial;
  auto result = io::write_session(partial, live);
  check(result.discarded_tail_events == 2 &&
            Session::parse(bytes(partial.str())).event_count() == chunk,
        "pending tail export reloads safely");
  auto header_only = io::crop_events(source, 0, 0);
  check(header_only.events.empty() && header_only.bootstrap_event_count == 0,
        "empty zero prefix");
}
void channels() {
  Fixture f(ByteOrder::little);
  auto cls = f.name("Memory", "test");
  f.emit(CreateClass{cls});
  auto prop = f.str("Events"), sem = f.str("memory event stream");
  auto type = f.name("PvdU8");
  f.emit(CreateProperty{cls, prop, sem, type, 2, {}});
  f.emit(CreateInstance{cls, 7});
  f.emit(SetPropertyValue{7, prop, type, 1, {0}});
  const auto begin = f.events.size();
  f.emit(SetPropertyValue{7, prop, type, 1, {1}});
  Session s(f.header, f.events);
  rejects([&] { io::crop_events(s, begin, s.event_count()); });
  io::CropOptions options;
  options.channel_bootstrap.push_back(
      {1, 7, 0, "Events", "memory event stream", {9}, false, {}});
  auto crop = io::crop_events(s, begin, s.event_count(), options);
  Session loaded(crop.header, crop.events);
  check(
      loaded.channels().size() == 2 &&
          loaded.channels()[0].value.raw_bytes() == Bytes{9} &&
          loaded.channels()[1].value.raw_bytes() == Bytes{1},
      "explicit provider replaces last raw packet and preserves source order");
  check(loaded.channels()[0].stream_id == 1, "provider source stream identity");
  options.channel_bootstrap[0].instance_generation = 1;
  rejects([&] { io::crop_events(s, begin, s.event_count(), options); });
  options.channel_bootstrap[0].instance_generation = 0;
  options.channel_bootstrap[0].partial = true;
  options.channel_bootstrap[0].warnings = {"unknown free before crop"};
  rejects([&] { io::crop_events(s, begin, s.event_count(), options); });
  options.allow_partial_analysis = true;
  auto partial = io::crop_events(s, begin, s.event_count(), options);
  check(partial.warnings.size() == 2,
        "partial source warning provenance retained");
  unsigned warnings = 0;
  for (const auto &event : partial.events)
    if (const auto *error = std::get_if<ErrorMessage>(&event.payload)) {
      check(error->code == 2, "partial warning code");
      ++warnings;
    }
  check(warnings == 2, "partial provenance survives raw event format");
}
struct TempDir {
  std::filesystem::path path;
  std::filesystem::path parent;
  bool owned = false;
  TempDir() {
    parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
    path =
        parent /
        std::filesystem::u8path(
            "pvd-writer-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    path = std::filesystem::absolute(path).lexically_normal();
    check(path.parent_path() == parent && path.filename().u8string().find("pvd-writer-") == 0,
          "test temporary directory escaped its intended parent");
    owned = std::filesystem::create_directory(path);
    check(owned, "temporary writer directory already exists");
  }
  ~TempDir() {
    std::error_code ec;
    if (owned && path.parent_path() == parent && path.filename().u8string().find("pvd-writer-") == 0)
      std::filesystem::remove_all(path, ec);
  }
};
void files() {
  TempDir dir;
  Fixture f(ByteOrder::little);
  f.setup();
  f.frame(0);
  Session source(f.header, f.events);
  const auto path = dir.path / std::filesystem::u8path(u8"保存🚀.pxd2");
  auto written = io::save_session(path.u8string(), source);
  check(written.state == io::WriteState::completed &&
            std::filesystem::exists(path),
        "UTF8 save publishes completed capture");
  std::ifstream in(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)), {});
  in.close();
  same_snapshot(source.latest(), Session::parse(bytes(content)).latest());
  same_snapshot(source.latest(), Session::load_file(path.u8string()).latest());
  rejects([&] { io::save_session(path.u8string(), source); });
  check(std::filesystem::file_size(path) == content.size(),
        "exclusive destination unchanged");
  io::WriteOptions cancel;
  std::size_t polls = 0;
  cancel.cancelled = [&] { return ++polls > 10; };
  const auto cancelled = dir.path / "cancel.pxd2";
  auto stopped = io::save_session(cancelled.u8string(), source, cancel);
  check(stopped.state == io::WriteState::cancelled &&
            !std::filesystem::exists(cancelled),
        "cancelled save leaves no completed file");
  io::WriteOptions fail;
  fail.max_output_bytes = 29;
  const auto failed = dir.path / "failed.pxd2";
  rejects([&] { io::save_session(failed.u8string(), source, fail); });
  check(!std::filesystem::exists(failed),
        "failed save leaves no completed file");
  fail.cancelled = {};
  rejects([&] { io::save_session(std::string("bad\0path", 8), source); });
  rejects([&] { io::save_session(path.u8string(), source, fail, true); });
  check(std::filesystem::file_size(path) == content.size(),
        "failed overwrite leaves old destination intact");
  auto overwrite = io::save_session(path.u8string(), source, {}, true);
  check(overwrite.state == io::WriteState::completed,
        "explicit overwrite succeeds");
  for (const auto &entry : std::filesystem::directory_iterator(dir.path))
    check(entry.path().filename().u8string().find(".pvd-tmp-") ==
              std::string::npos,
          "private files cleaned after failure/cancel");
  std::ostringstream bad;
  bad.setstate(std::ios::badbit);
  rejects([&] { io::write_session(bad, source); });
  auto invalid = all_events()[0];
  std::get<StringHandleEvent>(invalid.payload).string = std::string("a\0b", 3);
  rejects([&] { io::encode_event(invalid, ByteOrder::little); });
  io::WriteOptions invalid_options;
  invalid_options.target_group_bytes = 0;
  rejects([&] {
    std::ostringstream s;
    io::write_session(s, source, invalid_options);
  });
}
void sdk(const char *path) {
  auto source = Session::load_file(path);
  io::WriteOptions o;
  o.target_group_bytes = 257;
  o.max_events_per_group = 17;
  auto restored = roundtrip(source, o);
  check(restored.event_count() == source.event_count(), "real SDK roundtrip");
  if (!source.frames().empty() &&
      source.channels().end() ==
          std::find_if(source.channels().begin(), source.channels().end(),
                       [](const auto &c) {
                         return c.semantic != "render event stream";
                       })) {
    const auto first = std::min<std::size_t>(1, source.frames().size() - 1),
               last =
                   std::min<std::size_t>(first + 2, source.frames().size() - 1);
    auto crop = io::crop_frames(source, first, last);
    Session selected(crop.header, crop.events);
    same_snapshot(source.snapshot_at_event(crop.source_begin_event_count),
                  selected.snapshot_at_event(crop.bootstrap_event_count));
    same_snapshot(source.snapshot_at_event(crop.source_end_event_count),
                  selected.latest());
    roundtrip(selected, o);
    std::cout << "SDK crop: "
              << crop.source_end_event_count - crop.source_begin_event_count
              << " selected + " << crop.bootstrap_event_count
              << " bootstrap events\n";
  }
  std::cout << "SDK writer: " << source.event_count() << " events, "
            << source.channels().size() << " channels, "
            << source.frames().size() << " frames\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 1) {
      raw();
      semantic(ByteOrder::little);
      semantic(ByteOrder::big);
      channels();
      files();
    } else
      for (int i = 1; i < argc; ++i)
        sdk(argv[i]);
    std::cout << "writer tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "writer tests failed: " << e.what() << '\n';
    return 1;
  }
}
