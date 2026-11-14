#include "load_job.hpp"
#include "pvd/writer.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using namespace pvd;
using namespace pvd::viewer;
using namespace std::chrono_literals;
void check(bool b, const char *text) {
  if (!b)
    throw std::runtime_error(text);
}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string, std::uint32_t> strings;
  std::uint32_t property = 0;
  WireName bytes_type;
  void emit(Payload p) {
    events.push_back({1, events.size() * 100, 0, std::move(p)});
  }
  std::uint32_t str(const std::string &s) {
    auto i = strings.find(s);
    if (i != strings.end())
      return i->second;
    auto h = static_cast<std::uint32_t>(strings.size() + 1);
    strings[s] = h;
    emit(StringHandleEvent{s, h});
    return h;
  }
  WireName type(const char *name) {
    auto ns = str("physx3"), n = str(name);
    return {ns, n};
  }
  Fixture() {
    header.stream_flags = 1;
    header.timestamp_numerator = 1;
    auto scene = type("PxScene"), data = type("Data");
    emit(CreateClass{scene});
    emit(CreateClass{data});
    property = str("raw");
    bytes_type = type("PvdU8");
    emit(CreateProperty{data, property, 0, bytes_type, 2, {}});
    emit(CreateInstance{scene, 1});
    emit(CreateInstance{scene, 2});
    emit(CreateInstance{data, 10});
    auto frame = str("frame"), after = str("validation_after_fetch");
    for (unsigned i = 0; i < 3; ++i) {
      emit(BeginSection{2, frame, events.size()});
      emit(EndSection{2, frame, events.size()});
      emit(BeginSection{1, frame, events.size()});
      emit(SetPropertyValue{10, property, bytes_type, 512,
                            Bytes(512, static_cast<std::uint8_t>(i))});
      emit(EndSection{1, frame, events.size()});
      emit(BeginSection{1, after, events.size()});
      emit(EndSection{1, after, events.size()});
    }
  }
  Session session() const { return Session(header, events); }
  std::unique_ptr<Document> document() const {
    auto d = std::make_unique<Document>();
    d->adopt(std::make_unique<Session>(header, events));
    return d;
  }
};
void raw(const std::filesystem::path &path, const StreamHeader &header,
         const std::vector<Event> &events) {
  std::ofstream out(path, std::ios::binary);
  writing::write_raw(out, header, events);
  check(bool(out), "write raw input fixture");
}
LoadRequest request(const std::filesystem::path &path,
                    SlotId target = 0xfedcba9876543210ull) {
  LoadRequest r;
  r.target_slot = target;
  r.source_path = path.u8string();
  r.read_buffer_bytes = 17;
  r.events_per_step = 1;
  return r;
}
LoadedDocument ready(LoadJob &job) {
  job.wait();
  auto status = job.status();
  if (status.stage != LoadStage::ready)
    throw std::runtime_error("load did not become ready: " + status.error);
  check(!job.running() && status.has_result &&
            status.bytes_read == status.file_size &&
            status.event_count == status.decoded_event_count &&
            status.fingerprint_events == status.event_count,
        "ready status/progress inconsistent");
  auto result = job.take_ready();
  check(result && result->document &&
            result->identity.event_count == status.event_count,
        "ready result missing");
  check(!job.take_ready() && !job.status().has_result,
        "ready result duplicated");
  return std::move(*result);
}
void same(const Document &expected, const LoadedDocument &actual) {
  check(actual.document->loaded() && !actual.document->live() &&
            actual.document->session()->finished(),
        "candidate did not become offline/finished");
  check(actual.document->session()->event_count() ==
                expected.session()->event_count() &&
            actual.document->scenes().size() == expected.scenes().size() &&
            actual.document->scene_id() == expected.scene_id() &&
            actual.document->timeline().size() == expected.timeline().size() &&
            actual.document->frame_index() == expected.frame_index() &&
            actual.document->frame()->snapshot.event_count ==
                expected.frame()->snapshot.event_count &&
            actual.document->frame()->draws.size() ==
                expected.frame()->draws.size(),
        "streaming document differs from normal offline load");
  check(actual.identity == workspace_source_identity(*expected.session()),
        "worker canonical fingerprint differs");
}
void fragmented_and_retry(const std::filesystem::path &path) {
  Document expected;
  expected.open_file(path.u8string());
  LoadJob job;
  check(!job.running() && job.status().stage == LoadStage::idle &&
            !job.take_ready(),
        "new loader not idle");
  for (unsigned i = 0; i < 3; ++i) {
    job.start(request(path, 123 + i));
    auto result = ready(job);
    check(result.target_slot == 123 + i &&
              result.source_path == path.u8string(),
          "stable target/path lost");
    same(expected, result);
  }
}
void cancellation_and_slot_protection(const std::filesystem::path &path) {
  Fixture f;
  Workspace workspace;
  auto target = workspace.add(f.document(), "old.pxd2"),
       other = workspace.create_empty();
  auto *slot = workspace.find(target);
  auto *original = &slot->document();
  auto *session = original->session();
  auto frame = original->frame();
  check(workspace.lock(target, SlotLock::loading),
        "target loading lock failed");
  LoadJob job;
  auto r = request(path, target);
  struct Gate {
    std::promise<void> entered, release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> once{false};
  };
  Gate gate;
  auto entered = gate.entered.get_future();
  std::atomic<bool> worker_callback{false};
  const auto owner = std::this_thread::get_id();
  r.cancelled = [&] {
    worker_callback = std::this_thread::get_id() != owner;
    if (job.status().stage == LoadStage::fingerprinting &&
        !gate.once.exchange(true)) {
      gate.entered.set_value();
      gate.released.wait();
    }
    return false;
  };
  job.start(r);
  struct Cleanup {
    LoadJob &job;
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
  } cleanup{job, gate};
  if (entered.wait_for(5s) != std::future_status::ready) {
    cleanup.release();
    job.cancel();
    job.wait();
    throw std::runtime_error("fingerprint cancellation checkpoint not reached");
  }
  auto progress = job.status();
  check(worker_callback && progress.stage == LoadStage::fingerprinting &&
            progress.bytes_read == progress.file_size &&
            progress.event_count == f.events.size(),
        "late load progress not measured correctly");
  check(!job.take_ready(), "nonblocking take returned unfinished candidate");
  check(workspace.activate(other) && &slot->document() == original &&
            slot->document().session() == session &&
            slot->document().frame() == frame,
        "background load mutated or redirected original slot");
  bool busy = false;
  try {
    job.start(request(path, target));
  } catch (const std::logic_error &) {
    busy = true;
  }
  check(busy, "running loader replaced by another start");
  job.cancel();
  check(job.status().cancel_requested, "cancel request not visible");
  cleanup.release();
  job.wait();
  check(job.status().stage == LoadStage::cancelled && !job.take_ready() &&
            &slot->document() == original &&
            slot->document().session() == session,
        "cancelled load exposed partial document or changed original");
  auto retry = request(path, target);
  job.start(retry);
  auto result = ready(job);
  check(workspace.unlock(target, SlotLock::loading), "target unlock failed");
  check(workspace.replace(result.target_slot, result.document,
                          result.source_path, nullptr, result.identity),
        "ready candidate could not replace stable target");
  check(workspace.active_id() == other && workspace.find(target) == slot &&
            slot->document().session() != session,
        "ready application replaced active rather than target slot");
}
void failed(LoadJob &job, const LoadRequest &r) {
  job.start(r);
  job.wait();
  check(job.status().stage == LoadStage::failed &&
            !job.status().error.empty() && !job.take_ready(),
        "failed load exposed result or hid error");
}
void errors_and_limits(const std::filesystem::path &dir,
                       const std::filesystem::path &valid) {
  LoadJob job;
  failed(job, request(dir / "missing.pxd2"));
  failed(job, request(dir));
  auto small = request(valid);
  small.max_file_bytes = 1;
  failed(job, small);
  check(job.status().file_size && *job.status().file_size > 1 &&
            job.status().bytes_read == 0,
        "size limit did not reject before reading");
  auto invalid = request(valid);
  invalid.read_buffer_bytes = 0;
  failed(job, invalid);
  invalid = request(valid);
  invalid.events_per_step = 0;
  failed(job, invalid);
  invalid = request(valid, 0);
  failed(job, invalid);
  invalid = request(valid);
  invalid.source_path = std::string("bad\0name", 8);
  failed(job, invalid);
  invalid = request(valid);
  invalid.source_path = std::string("\xc0\xaf", 2);
  failed(job, invalid);
  auto cancelled = request(valid);
  cancelled.cancelled = [] { return true; };
  job.start(cancelled);
  job.wait();
  check(job.status().stage == LoadStage::cancelled &&
            job.status().bytes_read == 0 && !job.take_ready(),
        "external early cancellation ignored");
  auto limits = request(valid);
  limits.session_options.decode_limits.max_total_events = 1;
  failed(job, limits);
  check(job.status().error_byte_offset.has_value(),
        "decoder limit error lost byte offset");
  std::ifstream input(valid, std::ios::binary);
  Bytes data{std::istreambuf_iterator<char>(input), {}};
  check(data.size() > 28, "input fixture size");
  data.pop_back();
  auto truncated = dir / "truncated.pxd2";
  {
    std::ofstream out(truncated, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()), data.size());
  }
  failed(job, request(truncated));
  check(job.status().error_byte_offset.has_value(),
        "truncated group error lost offset");
  auto legacy = dir / "legacy.pxd2";
  Bytes old(28, 0);
  for (unsigned i = 0; i < 4; ++i)
    old[i] = static_cast<std::uint8_t>(legacy_container_magic >> (8 * i));
  {
    std::ofstream out(legacy, std::ios::binary);
    out.write(reinterpret_cast<const char *>(old.data()), old.size());
  }
  failed(job, request(legacy));
  Fixture f;
  auto tail = f.events;
  tail.push_back(
      {1, 0, 0, BeginSetPropertyValue{10, f.property, f.bytes_type}});
  tail.push_back({1, 0, 0, AppendPropertyValueData{{1, 2}, 2}});
  auto pending = dir / "pending.pxd2";
  raw(pending, f.header, tail);
  failed(job, request(pending));
  check(job.status().error_byte_offset.has_value(),
        "unfinished Session boundary error lost offset");
  auto mismatch = f.events;
  mismatch.push_back(
      {1, 0, 0, SetPropertyValue{10, f.property, f.bytes_type, 2, {1, 2, 3}}});
  auto semantic = dir / "semantic.pxd2";
  raw(semantic, f.header, mismatch);
  failed(job, request(semantic));
  job.start(request(valid));
  auto recovered = ready(job);
  check(recovered.document->session()->event_count() == f.events.size(),
        "loader did not recover after failures");
}
void actual_sdk(const char *path) {
  Document expected;
  expected.open_file(path);
  LoadJob job;
  LoadRequest r;
  r.target_slot = 44;
  r.source_path = path;
  job.start(r);
  auto loaded = ready(job);
  same(expected, loaded);
  std::cout << "SDK streaming load: " << job.status().bytes_read << " bytes / "
            << loaded.identity.event_count << " events\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    auto dir =
        std::filesystem::current_path() / "build" /
        ("load-job-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    auto path = dir / std::filesystem::u8path(u8"\u6355\u83b7-\U0001F680.pxd2");
    Fixture f;
    raw(path, f.header, f.events);
    fragmented_and_retry(path);
    cancellation_and_slot_protection(path);
    errors_and_limits(dir, path);
    if (argc > 1)
      actual_sdk(argv[1]);
    std::cout << "load job tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
