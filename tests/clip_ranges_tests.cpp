#include "clip_range_fixture.hpp"
#include "pvd/clips.hpp"
#include "pvd/writer.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace c = pvd::clips;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F &&fn, const char *message) {
  bool caught = false;
  try {
    fn();
  } catch (const std::exception &) {
    caught = true;
  }
  check(caught, message);
}
void editing(ByteOrder order) {
  clip_test::Fixture fixture(order);
  auto source = fixture.session();
  const auto original = source.events().size();
  auto ranges = c::Catalog::create(source, {1, 0});
  check(ranges.state().binding.native_frame_count == 8 &&
            ranges.state().binding.available.first == 0 &&
            ranges.state().binding.available.last == 5 &&
            ranges.selected()->frames.count() == 6,
        "catalog retains original native ordinal across reused scene ID");
  ranges.rename(source, 1, u8"故障 / 名称 ### literal");
  auto ids = ranges.split(source, 2);
  check(ranges.state().clips.size() == 2 && !ranges.find(1) &&
            ids.first != ids.second && ranges.selected()->id == ids.first &&
            ranges.find(ids.first)->frames.first == 0 &&
            ranges.find(ids.first)->frames.last == 2 &&
            ranges.find(ids.second)->frames.first == 3 &&
            ranges.find(ids.second)->frames.last == 5,
        "split partitions original range without duplicate/dropped frames");
  ranges.trim_left(source, 1);
  ranges.trim_right(source, 1);
  check(ranges.selected()->frames.count() == 1 &&
            ranges.selected()->id == ids.first,
        "trims retain playhead and stable selected clip ID");
  rejects([&] { ranges.split(source, 1); },
          "one-frame split rejects empty side");
  rejects([&] { ranges.trim_left(source, 0); }, "outside trim rejected");
  ranges.select(source, ids.second);
  auto restored = c::Catalog::restore(source, ranges.state());
  check(restored.selected()->id == ids.second &&
            restored.find(ids.first)->frames.count() == 1,
        "persistent values preserve named clips and selection");
  const auto next = ranges.state().next_id;
  check(ranges.erase(source, ids.second) && ranges.selected()->id == ids.first,
        "delete retains adjacent selected clip");
  const auto added = ranges.add(source, "Another", {2, 4});
  check(added == next && added > ids.second,
        "new range never reuses a deleted stable ID");
  rejects([&] { ranges.add(source, "Cross generation", {5, 6}); },
          "ranges cannot cross reused scene lifetime");
  auto later = c::Catalog::create(source, {1, 1});
  check(later.selected()->frames.first == 6 &&
            later.selected()->frames.last == 7,
        "new generation keeps whole scene's native ordinal");
  auto other = c::Catalog::create(source, {2, 0});
  check(other.selected()->frames.count() == 3,
        "second scene uses its own native rather than global frame indices");
  check(source.event_count() == original,
        "non-destructive editing never changes source events");

  // The existing crop writer consumes the same scene ordinals after translating
  // to global FrameMarker positions; it retains exact property values.
  std::vector<std::size_t> native;
  for (std::size_t i = 0; i < source.frames().size(); ++i)
    if (source.frames()[i].scene_id == 1)
      native.push_back(i);
  const auto clip = *ranges.find(added);
  c::validate_clip(source, clip);
  auto cropped = writing::crop_frames(source, native[clip.frames.first],
                                      native[clip.frames.last]);
  Session reopened(cropped.header, cropped.events);
  std::size_t count = 0;
  for (const auto &frame : reopened.frames())
    if (frame.scene_id == 1)
      ++count;
  check(count == 3, "named range produces a real three-frame crop");
  const auto expected = source.snapshot_at_frame(native[clip.frames.last])
                            .instance(20)
                            ->property("Counter")
                            ->raw_bytes();
  const auto actual = reopened.snapshot_at_frame(reopened.frames().size() - 1)
                          .instance(20)
                          ->property("Counter")
                          ->raw_bytes();
  check(actual == expected, "range save/load preserves exact u64 state");
  std::cout << "Clip editing/source/lifetime/roundtrip order="
            << (order == ByteOrder::little ? "LE" : "BE") << " passed\n";
}
void live_and_identity() {
  clip_test::Fixture fixture;
  Session live(fixture.header);
  live.append(std::vector<Event>(fixture.events.begin(),
                                 fixture.events.begin() + fixture.open_frame));
  auto ranges = c::Catalog::create(live, {1, 0});
  const auto old = *ranges.selected();
  check(old.frames.count() == 3,
        "open native frame is not admitted as range end");
  live.append(std::vector<Event>(fixture.events.begin() + fixture.open_frame,
                                 fixture.events.end()));
  ranges.validate(live);
  c::validate_clip(live, old);
  ranges.refresh(live);
  check(
      ranges.state().binding.available.last == 5 &&
          ranges.selected()->frames.count() == 3 &&
          ranges.selected()->binding.source == old.binding.source,
      "live refresh extends availability without rewriting sealed clip values");
  ranges.add(live, "Received later", {3, 5});
  check(ranges.selected()->binding.source.event_count >
            old.binding.source.event_count,
        "newly closed frames get their own explicit source seal");
  live.finish();
  auto changed = fixture.events;
  std::get<SetPropertyValue>(changed[fixture.first_value].payload).data[0] ^= 1;
  Session replacement(fixture.header, changed);
  check(replacement.event_count() == live.event_count(),
        "same-sized replacement fixture");
  rejects([&] { ranges.validate(replacement); },
          "same path/count changed content is rejected");
  rejects([&] { c::Catalog::restore(replacement, ranges.state()); },
          "workspace restore verifies original sealed source content");
  auto truncated = std::vector<Event>(
      fixture.events.begin(), fixture.events.begin() + fixture.live_seed - 1);
  Session short_source(fixture.header, truncated);
  rejects([&] { c::validate_clip(short_source, old); },
          "shorter replacement source rejected");
  auto pending_events = fixture.events;
  const auto property = fixture.strings.at("Counter");
  const WireName datatype{fixture.strings.at("physx3"),
                          fixture.strings.at("PvdU64")};
  Session pending(fixture.header);
  pending.append(pending_events);
  pending.append(
      {Event{17, 0, 0, BeginSetPropertyValue{20, property, datatype}}});
  auto safe = c::Catalog::create(pending, {1, 1});
  check(safe.state().binding.source.event_count == pending_events.size(),
        "latest safe prefix excludes an unfinished property tail");
  rejects([&] { c::source_identity(pending, pending.event_count()); },
          "explicit unsealed property prefix rejected");
  std::cout
      << "Live sealed clips/append/refresh/replacement validation passed\n";
}
void invalid_values() {
  clip_test::Fixture fixture;
  auto source = fixture.session();
  auto ranges = c::Catalog::create(source, {1, 0});
  auto state = ranges.state();
  state.clips[0].binding.available.last = 7;
  rejects([&] { c::Catalog::restore(source, state); },
          "forged lifetime span rejected");
  state = ranges.state();
  state.clips.push_back(state.clips[0]);
  rejects([&] { c::Catalog::restore(source, state); },
          "duplicate stable clip ID rejected");
  state = ranges.state();
  state.selected = 99;
  rejects([&] { c::Catalog::restore(source, state); },
          "missing selected ID rejected");
  rejects([&] { ranges.rename(source, 1, std::string("\xf0\x80\x80\x80", 4)); },
          "invalid UTF8 name rejected");
  rejects([&] { ranges.add(source, "Huge", {0, SIZE_MAX}); },
          "overflowing frame span rejected");
  rejects([&] { c::Catalog::create(source, {99, 0}); },
          "unrecorded scene rejected");
  c::Limits limits;
  limits.max_clips = 2;
  auto bounded = c::Catalog::create(source, {1, 0}, SIZE_MAX, limits);
  auto ids = bounded.split(source, 2);
  const auto before = bounded.state().next_id;
  rejects([&] { bounded.add(source, "Third", {0, 0}); },
          "catalog budget enforced");
  check(bounded.state().next_id == before &&
            bounded.state().clips.size() == 2 &&
            bounded.selected()->id == ids.first,
        "rejected edits leave all clip values and stable IDs intact");
  bool cancel = false;
  limits.checkpoint = [&](std::size_t, std::size_t) {
    if (cancel)
      throw std::runtime_error("cancel identity validation");
  };
  auto cancellable = c::Catalog::create(source, {1, 0}, SIZE_MAX, limits);
  cancel = true;
  rejects([&] { cancellable.split(source, 2); },
          "identity preparation can be cancelled");
  check(cancellable.state().clips.size() == 1 &&
            cancellable.state().next_id == 2,
        "cancelled preparation does not publish partial editing state");
  std::cout << "Clip corruption/UTF8/limits/strong-failure tests passed\n";
}
} // namespace
void batched_restore() {
  clip_test::Fixture fixture;
  auto session=fixture.session();
  auto original=c::Catalog::create(session,{1,0}).state();
  auto corrupt=original;corrupt.binding.source.fingerprint^=123;
  auto malformed=original;malformed.clips.front().frames.last=SIZE_MAX;
  const auto result=c::restore_catalogs(session,{original,corrupt,malformed,original});
  check(result.size()==4&&result[0].catalog&&!result[1].catalog&&!result[2].catalog&&
        result[3].catalog&&!result[1].error.empty()&&!result[2].error.empty(),
        "batch restore failed to isolate invalid source and range entries");
  c::Limits limits;std::size_t checkpoints=0;
  limits.checkpoint=[&](std::size_t,std::size_t){++checkpoints;};
  c::restore_catalogs(session,{original},limits);const auto once=checkpoints;
  checkpoints=0;c::restore_catalogs(session,std::vector<c::State>(32,original),limits);
  check(checkpoints==once,"batch rehashed the entire recording for every catalog");
  limits.checkpoint=[](std::size_t,std::size_t){throw c::Error("cancel whole batch");};
  rejects([&]{c::restore_catalogs(session,{original,corrupt},limits);},
          "batch swallowed caller cancellation as a per-entry error");
  limits={};limits.max_catalogs=1;
  rejects([&]{c::restore_catalogs(session,{original,original},limits);},
          "batch catalog limit ignored");
}
int main() {
  try {
    editing(ByteOrder::little);
    editing(ByteOrder::big);
    live_and_identity();
    invalid_values();
    batched_restore();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
