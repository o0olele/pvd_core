#include "export_job.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <pvd/memory.hpp>
#include <pvd/profile.hpp>
#include <stdexcept>
#include <thread>

namespace {
using namespace pvd;
using namespace pvd::viewer;
using namespace std::chrono_literals;
void check(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  std::uint32_t property = 0;
  WireName bytes_type;
  void emit(Payload value) {
    events.push_back(
        {1, events.size() * 10, events.size() * 4, std::move(value)});
  }
  std::uint32_t str(const std::string &s) {
    auto found = strings.find(s);
    if (found != strings.end())
      return found->second;
    auto h = static_cast<std::uint32_t>(strings.size() + 1);
    strings[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *n) {
    auto ns = str("physx3"), h = str(n);
    return {ns, h};
  }
  static void u(Bytes &b, std::uint64_t n, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
      b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
  }
  static void string(Bytes &b, const char *text, std::uint32_t h) {
    auto n = std::char_traits<char>::length(text);
    u(b, 0x3ff1, 2);
    u(b, n + 1, 4);
    b.insert(b.end(), text, text + n + 1);
    u(b, h, 4);
  }
  static void allocation(Bytes &b, std::uint64_t address, unsigned size) {
    u(b, 0x3ff2, 2);
    u(b, size, 4);
    u(b, 1, 4);
    u(b, 2, 4);
    u(b, 42, 4);
    u(b, address, 8);
  }
  static void free(Bytes &b, std::uint64_t address) {
    u(b, 0x33, 2);
    u(b, address, 8);
  }
  void packet(Bytes bytes) {
    auto size = static_cast<std::uint32_t>(bytes.size());
    emit(SetPropertyValue{10, property, bytes_type, size, std::move(bytes)});
  }
  explicit Fixture(bool partial = false, bool with_profile = false) {
    header.stream_flags = 1;
    header.timestamp_numerator = 1;
    header.timestamp_denominator = 1;
    auto scene = type("PxScene"), buffer = type("MemoryFixture");
    emit(CreateClass{scene});
    emit(CreateClass{buffer});
    bytes_type = type("PvdU8");
    property = str("events");
    auto semantic = str("memory event stream");
    emit(CreateProperty{buffer, property, semantic, bytes_type, 2, {}});
    emit(CreateInstance{scene, 1});
    emit(CreateInstance{scene, 2});
    emit(CreateInstance{buffer, 10});
    Bytes initial;
    string(initial, "ExportFixture", 1);
    string(initial, "fixture.cpp", 2);
    allocation(initial, 0x1000, 10);
    if (partial)
      free(initial, 0xdead);
    packet(std::move(initial));
    std::uint32_t profile_property = 0;
    auto profile_packet = [&](unsigned tag, std::uint64_t tick) {
      Bytes b;
      u(b, tag, 1);
      u(b, 15, 1);
      u(b, 1, 2);
      u(b, 7, 4);
      u(b, 9, 8);
      u(b, 4, 1);
      u(b, 3, 1);
      u(b, tick, 8);
      auto size = static_cast<std::uint32_t>(b.size());
      emit(SetPropertyValue{30, profile_property, bytes_type, size,
                            std::move(b)});
    };
    if (with_profile) {
      auto profile_class = type("ProfileBuffer");
      emit(CreateClass{profile_class});
      profile_property = str("profile_events");
      auto sem = str("profile event stream");
      emit(CreateProperty{
          profile_class, profile_property, sem, bytes_type, 2, {}});
      emit(AddProfileZone{30, "Export zone"});
      emit(CreateInstance{profile_class, 30});
      emit(AddProfileZoneEvent{30, "Spanning scope", 1, true});
      profile_packet(1, 100);
    }
    const auto frame = str("frame");
    for (unsigned step = 0; step != 3; ++step) {
      // Interleaving catches accidental use of global rather than scene-local
      // indices.
      emit(BeginSection{2, frame, events.size() * 10});
      emit(EndSection{2, frame, events.size() * 10});
      emit(BeginSection{1, frame, events.size() * 10});
      Bytes value;
      if (step == 0)
        allocation(value, 0x2000, 20);
      if (step == 1) {
        free(value, 0x1000);
        allocation(value, 0x3000, 30);
      }
      if (step == 2)
        free(value, 0x2000);
      packet(std::move(value));
      if (with_profile && step == 1)
        profile_packet(2, 160);
      emit(ErrorMessage{2, "FRAME_" + std::to_string(step), "fixture.cpp",
                        step});
      emit(EndSection{1, frame, events.size() * 10});
    }
  }
  Session session() const { return Session(header, events); }
};
std::string read(const std::filesystem::path &file) {
  std::ifstream in(file, std::ios::binary);
  check(bool(in), "read output");
  return {std::istreambuf_iterator<char>(in), {}};
}
void write(const std::filesystem::path &file, const char *value) {
  std::ofstream out(file, std::ios::binary);
  out << value;
  check(bool(out), "write original sentinel");
}
void completed(ExportJob &job) {
  job.wait();
  const auto status = job.status();
  if (status.stage != ExportStage::completed)
    throw std::runtime_error("export did not complete: " + status.error);
  check(!job.running() && status.has_result && status.elapsed_seconds >= 0,
        "terminal export status inconsistent");
}
ExportRequest whole(const std::filesystem::path &file) {
  ExportRequest r;
  r.destination = file.u8string();
  return r;
}
ExportRequest range(const std::filesystem::path &file) {
  auto r = whole(file);
  r.mode = ExportMode::native_frame_range;
  r.scene_id = 1;
  r.first_native_frame = 1;
  r.last_native_frame = 2;
  return r;
}
void full_and_safe_tail(const std::filesystem::path &dir) {
  Fixture f;
  auto session = f.session();
  ExportJob job;
  check(job.status().stage == ExportStage::idle && !job.running(),
        "new job not idle");
  const auto destination =
      dir / std::filesystem::u8path(u8"\u5bfc\u51fa-\U0001F680.pxd2");
  job.start(session, whole(destination));
  completed(job);
  auto copy = Session::load_file(destination.u8string());
  check(copy.event_count() == session.event_count() &&
            copy.frames().size() == 6 &&
            copy.channels().size() == session.channels().size(),
        "whole recording did not reopen faithfully");
  for (std::size_t i = 0; i < copy.events().size(); ++i)
    check(writing::encode_event(copy.events()[i], copy.header().byte_order) ==
              writing::encode_event(session.events()[i],
                                    session.header().byte_order),
          "whole typed event payload changed");
  // The job API also supports a borrowed stopped/failed Session with an open
  // property tail; safe_prefix trims the uncommitted transaction, not good
  // data.
  Session pending(f.header);
  pending.append(f.events);
  pending.append(
      {{1, 0, 0, BeginSetPropertyValue{10, f.property, f.bytes_type}},
       {1, 0, 0, AppendPropertyValueData{{1, 2}, 2}}});
  auto tail = whole(dir / "safe-tail.pxd2");
  job.start(pending, tail);
  completed(job);
  auto status = job.status();
  check(status.result.discarded_tail_events == 2 &&
            status.result.requested_event_count == f.events.size() + 2 &&
            status.result.event_count == f.events.size(),
        "discarded tail not accurately reported");
  check(Session::load_file(tail.destination).event_count() == f.events.size(),
        "safe prefix file did not reopen");
}
void failure_and_cancel(const std::filesystem::path &dir) {
  Fixture f;
  auto session = f.session();
  ExportJob job;
  const auto existing = dir / "preserve.pxd2";
  write(existing, "ORIGINAL");
  job.start(session, whole(existing));
  job.wait();
  check(job.status().stage == ExportStage::failed &&
            !job.status().error.empty() && read(existing) == "ORIGINAL",
        "default overwrite protection failed");
  auto fail = whole(existing);
  fail.overwrite = true;
  fail.write_options.max_output_bytes = 1;
  job.start(session, fail);
  job.wait();
  check(job.status().stage == ExportStage::failed &&
            read(existing) == "ORIGINAL",
        "write failure changed original destination");
  struct Gate {
    std::promise<void> entered, release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> once{false};
  };
  Gate gate;
  auto entered = gate.entered.get_future();
  auto cancel = whole(existing);
  cancel.overwrite = true;
  struct Cleanup {
    ExportJob &job;
    Gate &gate;
    bool released = false;
    void release() {
      if (!released) {
        released = true;
        gate.release.set_value();
      }
    }
    ~Cleanup() {
      job.cancel();
      release();
      job.wait();
    }
  };
  const auto main_thread = std::this_thread::get_id();
  std::atomic<bool> callback_on_worker{false};
  cancel.write_options.cancelled = [&] {
    callback_on_worker = std::this_thread::get_id() != main_thread;
    if (!gate.once.exchange(true)) {
      gate.entered.set_value();
      gate.released.wait();
    }
    return false;
  };
  job.start(session, cancel);
  Cleanup cleanup{job, gate};
  if (entered.wait_for(5s) != std::future_status::ready) {
    cleanup.release();
    job.cancel();
    job.wait();
    throw std::runtime_error("writer cancellation gate not reached");
  }
  check(job.running() && job.status().stage == ExportStage::writing &&
            callback_on_worker,
        "worker stage/callback thread incorrect");
  bool busy_rejected = false;
  try {
    job.start(session, whole(dir / "concurrent.pxd2"));
  } catch (const std::logic_error &) {
    busy_rejected = true;
  }
  check(busy_rejected, "concurrent job replaced borrowed source");
  job.cancel();
  check(job.status().cancel_requested, "cancel request invisible");
  cleanup.release();
  job.wait();
  check(job.status().stage == ExportStage::cancelled && !job.running() &&
            read(existing) == "ORIGINAL",
        "cancelled job published or damaged output");
  for (const auto &file : std::filesystem::directory_iterator(dir))
    check(file.path().filename().u8string().find(".pvd-tmp-") ==
              std::string::npos,
          "private temporary file leaked");
  job.start(session, whole(dir / "after-cancel.pxd2"));
  completed(job);
  check(!job.status().cancel_requested &&
            Session::load_file((dir / "after-cancel.pxd2").u8string())
                    .event_count() == session.event_count(),
        "job could not be reused after cancellation");
  auto invalid = range(dir / "invalid-range.pxd2");
  invalid.last_native_frame = 100;
  job.start(session, invalid);
  job.wait();
  check(job.status().stage == ExportStage::failed &&
            !std::filesystem::exists(
                std::filesystem::u8path(invalid.destination)),
        "invalid frame range published output");
}
void frame_crop(const std::filesystem::path &dir) {
  Fixture f;
  auto session = f.session();
  ExportJob job;
  auto request = range(dir / "range.pxd2");
  job.start(session, request);
  job.wait();
#ifdef PVD_VIEWER_HAS_ANALYSIS
  completed(job);
  auto cropped = Session::load_file(request.destination);
  std::size_t main = 0, other = 0;
  for (const auto &frame : cropped.frames()) {
    if (frame.scene_id == 1)
      ++main;
    if (frame.scene_id == 2)
      ++other;
  }
  check(main == 2 && other == 1, "native range did not use current scene "
                                 "indices or lost interleaved streams");
  memory::Index index(cropped);
  auto state = index.snapshot_at_event(cropped.event_count());
  check(state.complete && state.active.size() == 1 &&
            state.active[0].address == 0x3000 &&
            state.totals.current_bytes == 30 &&
            state.totals.allocated_bytes == 60 &&
            state.totals.freed_bytes == 30 && state.totals.peak_bytes == 50,
        "asynchronous crop memory bootstrap wrong");
  Fixture with_profile(false, true);
  auto profile_source = with_profile.session();
  auto profiled = range(dir / "profile-range.pxd2");
  job.start(profile_source, profiled);
  completed(job);
  auto profile_saved = Session::load_file(profiled.destination);
  profile::Index profile_index(profile_saved);
  check(
      profile_index.intervals().size() == 1 &&
          profile_index.intervals()[0].duration_ticks == 60 &&
          profile_index.intervals()[0].begin_ticks == 100 &&
          profile_index.intervals()[0].end_ticks == 160 &&
          profile_index.intervals()[0].thread_id == 7 &&
          profile_index.intervals()[0].context_id == 9,
      "asynchronous profile open-scope bootstrap lost start/context/duration");
  Fixture partial(true);
  auto incomplete = partial.session();
  auto strict = range(dir / "strict.pxd2");
  job.start(incomplete, strict);
  job.wait();
  check(
      job.status().stage == ExportStage::failed &&
          !std::filesystem::exists(std::filesystem::u8path(strict.destination)),
      "strict memory crop silently accepted incomplete source");
  strict.destination = (dir / "partial.pxd2").u8string();
  strict.allow_partial_memory = true;
  job.start(incomplete, strict);
  completed(job);
  check(!job.status().warnings.empty(), "partial memory warnings not reported");
  auto saved = Session::load_file(strict.destination);
  bool warning = false;
  for (const auto &event : saved.events())
    if (const auto *e = std::get_if<ErrorMessage>(&event.payload))
      if (e->message.find("Partial crop analysis baseline") !=
          std::string::npos)
        warning = true;
  check(warning, "partial warning provenance not persisted");
#else
  check(job.status().stage == ExportStage::failed &&
            job.status().error.find("analysis") != std::string::npos,
        "analysis-disabled crop did not explain unsupported source");
#endif
}
} // namespace
int main() {
  try {
    const auto dir =
        std::filesystem::current_path() / "build" /
        ("export-job-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    full_and_safe_tail(dir);
    failure_and_cancel(dir);
    frame_crop(dir);
    std::cout << "export job tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
