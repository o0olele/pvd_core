#include "pvd/clips.hpp"
#include "pvd/writer.hpp"
#include <algorithm>
#include <set>

namespace pvd::clips {
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw Error(message);
}
void limits_valid(const Limits &l) {
  require(l.max_clips && l.max_name_bytes && l.max_event_bytes,
          "Clip limits must be nonzero");
}
void name_valid(const std::string &name, const Limits &limits) {
  require(!name.empty() && name.size() <= limits.max_name_bytes,
          "Clip name is empty or exceeds the UTF-8 byte limit");
  for (std::size_t at = 0; at < name.size();) {
    const auto a = static_cast<unsigned char>(name[at++]);
    require(a != 0, "Clip name contains NUL");
    if (a < 0x80)
      continue;
    unsigned tail;
    std::uint32_t cp, minimum;
    if (a >= 0xc2 && a <= 0xdf) {
      tail = 1;
      cp = a & 31;
      minimum = 0x80;
    } else if (a >= 0xe0 && a <= 0xef) {
      tail = 2;
      cp = a & 15;
      minimum = 0x800;
    } else if (a >= 0xf0 && a <= 0xf4) {
      tail = 3;
      cp = a & 7;
      minimum = 0x10000;
    } else
      throw Error("Clip name is not valid UTF-8");
    require(tail <= name.size() - at, "Clip name ends inside UTF-8 text");
    while (tail--) {
      const auto b = static_cast<unsigned char>(name[at++]);
      require((b & 0xc0) == 0x80, "Clip name is not valid UTF-8");
      cp = (cp << 6) | (b & 63);
    }
    require(cp >= minimum && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff),
            "Clip name contains an invalid Unicode scalar");
  }
}
std::size_t sealed(const Session &session, std::size_t prefix) {
  const auto safe = writing::safe_prefix(session, prefix);
  if (prefix != std::numeric_limits<std::size_t>::max())
    require(safe.event_count == prefix,
            "Clip source prefix ends inside a property transaction");
  return safe.event_count;
}
// Verify all stored seals in ONE forward pass. A long catalog must not hash the
// same recording once per clip when several ranges share one prefix.
std::map<std::size_t, std::uint64_t>
hash_prefixes(const Session &session, const std::set<std::size_t> &prefixes,
              const Limits &limits) {
  limits_valid(limits);
  std::map<std::size_t, std::uint64_t> result;
  if (prefixes.empty())
    return result;
  const auto total = *prefixes.rbegin();
  require(total <= session.event_count(),
          "Clip source has fewer events than its sealed prefix");
  std::uint64_t hash = 14695981039346656037ull;
  std::size_t done = 0;
  auto byte = [&](std::uint8_t value) {
    hash = (hash ^ value) * 1099511628211ull;
  };
  auto bytes = [&](const Bytes &data) {
    for (std::size_t at = 0; at < data.size();) {
      if (limits.checkpoint)
        limits.checkpoint(done, total);
      const auto end = at + std::min<std::size_t>(65536, data.size() - at);
      while (at < end)
        byte(data[at++]);
    }
  };
  auto integer = [&](std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
      byte(static_cast<std::uint8_t>(value >> (8 * i)));
  };
  bytes(writing::encode_header(session.header()));
  if (prefixes.count(0))
    result[0] = hash;
  for (; done < total;) {
    if (limits.checkpoint)
      limits.checkpoint(done, total);
    const auto &event = session.events()[done];
    integer(event.stream_id);
    integer(event.timestamp);
    bytes(writing::encode_event(event, session.header().byte_order,
                                limits.max_event_bytes));
    ++done;
    if (prefixes.count(done))
      result[done] = hash;
  }
  if (limits.checkpoint)
    limits.checkpoint(done, total);
  return result;
}
bool same(Span a, Span b) { return a.first == b.first && a.last == b.last; }
struct FrameIndex {
  const Session &session;
  SceneKey scene;
  std::vector<const FrameMarker *> frames;
  Span lifetime_span;
  FrameIndex(const Session &s, SceneKey key) : session(s), scene(key) {
    require(scene.id != 0, "Clip scene ID is zero");
    const ObjectLifetime *lifetime = nullptr;
    for (const auto &entry : session.lifetimes())
      if (entry.id == scene.id && entry.generation == scene.generation) {
        lifetime = &entry;
        break;
      }
    require(lifetime && session.metadata().is_a(lifetime->class_name,
                                                {"physx3", "PxScene"}),
            "Clip scene lifetime is absent or is not a native PxScene");
    bool any = false;
    for (const auto &frame : session.frames()) {
      if (frame.scene_id != scene.id)
        continue;
      const auto ordinal = frames.size();
      frames.push_back(&frame);
      if (lifetime->created_event >= frame.event_count ||
          (lifetime->destroyed_event &&
           *lifetime->destroyed_event < frame.event_count))
        continue;
      require(frame.begin_event_count != 0 &&
                  lifetime->created_event < frame.begin_event_count,
              "Native frame begins outside the selected scene lifetime");
      if (!any)
        lifetime_span.first = ordinal;
      lifetime_span.last = ordinal;
      any = true;
    }
    require(any, "This scene lifetime has no closed native frames");
  }
  Binding derive(SourceIdentity identity) const {
    require(identity.algorithm == 1 &&
                identity.event_count <= session.event_count(),
            "Clip source identity algorithm or event prefix is invalid");
    const auto end =
        std::upper_bound(frames.begin(), frames.end(), identity.event_count,
                         [](std::uint64_t prefix, const FrameMarker *frame) {
                           return prefix < frame->event_count;
                         });
    const auto count = static_cast<std::size_t>(end - frames.begin());
    require(count > lifetime_span.first,
            "This scene lifetime has no closed native frames at the prefix");
    return {identity,
            scene,
            {lifetime_span.first, std::min(lifetime_span.last, count - 1)},
            count};
  }
  void binding_valid(const Binding &binding) const {
    require(binding.scene == scene, "Clip belongs to another scene lifetime");
    const auto expected = derive(binding.source);
    require(binding.native_frame_count == expected.native_frame_count &&
                same(binding.available, expected.available),
            "Stored clip native-frame/lifetime bounds do not match the source");
  }
  void range_valid(const Clip &clip, const Limits &limits,
                   std::set<std::size_t> &boundaries) const {
    require(clip.id != 0, "Clip ID is zero");
    name_valid(clip.name, limits);
    binding_valid(clip.binding);
    require(clip.frames.first <= clip.frames.last &&
                clip.frames.first >= clip.binding.available.first &&
                clip.frames.last <= clip.binding.available.last,
            "Clip range crosses a scene lifetime or exceeds its sealed frames");
    const auto *first = frames.at(clip.frames.first),
               *last = frames.at(clip.frames.last);
    require(first->begin_event_count &&
                last->event_count <= clip.binding.source.event_count,
            "Clip endpoints are not sealed native frame markers");
    boundaries.insert(first->begin_event_count - 1);
    boundaries.insert(last->event_count);
  }
};
void safe_boundaries(const Session &session,
                     const std::set<std::size_t> &boundaries,
                     const Limits &limits) {
  if (boundaries.empty())
    return;
  const auto total = *boundaries.rbegin();
  require(total <= session.event_count(),
          "Clip boundary exceeds the received source");
  std::set<std::uint64_t> pending;
  for (std::size_t i = 0; i < total; ++i) {
    if (limits.checkpoint && i % 4096 == 0)
      limits.checkpoint(i, total);
    const auto &event = session.events()[i];
    if (std::holds_alternative<BeginSetPropertyValue>(event.payload) ||
        std::holds_alternative<BeginPropertyMessageGroup>(event.payload))
      require(pending.insert(event.stream_id).second,
              "Clip source has nested property transactions");
    else if (std::holds_alternative<EndSetPropertyValue>(event.payload) ||
             std::holds_alternative<EndPropertyMessageGroup>(event.payload))
      require(pending.erase(event.stream_id) != 0,
              "Clip source has an unmatched property transaction end");
    if (boundaries.count(i + 1))
      require(pending.empty(),
              "Clip boundary is inside a property transaction");
  }
}
void range_valid(const Session &session, const Clip &clip,
                 const Limits &limits) {
  std::set<std::size_t> boundaries;
  FrameIndex(session, clip.binding.scene).range_valid(clip, limits, boundaries);
  safe_boundaries(session, boundaries, limits);
}
void verify_sources(const Session &session,
                    const std::vector<SourceIdentity> &sources,
                    const Limits &limits,
                    std::set<std::size_t> boundaries = {}) {
  std::set<std::size_t> prefixes;
  for (const auto &source : sources) {
    require(source.algorithm == 1 &&
                source.event_count <= std::numeric_limits<std::size_t>::max(),
            "Stored clip source identity is invalid");
    const auto prefix = static_cast<std::size_t>(source.event_count);
    prefixes.insert(prefix);
  }
  boundaries.insert(prefixes.begin(), prefixes.end());
  safe_boundaries(session, boundaries, limits);
  const auto hashes = hash_prefixes(session, prefixes, limits);
  for (const auto &source : sources)
    require(hashes.at(static_cast<std::size_t>(source.event_count)) ==
                source.fingerprint,
            "Clip source content differs from the stored sealed prefix");
}
struct Verification {
  std::vector<SourceIdentity> sources;
  std::set<std::size_t> boundaries;
};
Verification state_structure(const State &state,const Limits &limits,
                              const FrameIndex &index) {
  limits_valid(limits);
  require(state.clips.size() <= limits.max_clips,
          "Clip catalog exceeds the clip count budget");
  Verification result;
  result.sources.push_back(state.binding.source);
  index.binding_valid(state.binding);
  std::set<Id> ids;
  for (const auto &clip : state.clips) {
    require(clip.binding.scene == state.binding.scene &&
                clip.binding.source.event_count <=
                    state.binding.source.event_count,
            "Clip belongs to another scene lifetime or a later catalog seal");
    require(ids.insert(clip.id).second && clip.id < state.next_id,
            "Clip catalog ID is duplicated or next ID is invalid");
    result.sources.push_back(clip.binding.source);
    index.range_valid(clip, limits, result.boundaries);
  }
  require(state.next_id != 0 &&
              (!state.selected || ids.count(*state.selected) != 0),
          "Clip selection or next ID is invalid");
  return result;
}
void state_valid(const Session &session,const State &state,const Limits &limits) {
  const FrameIndex index(session,state.binding.scene);
  auto requested=state_structure(state,limits,index);
  verify_sources(session,requested.sources,limits,std::move(requested.boundaries));
}
std::string split_name(const std::string &old, const char *suffix,
                       const Limits &limits) {
  const std::string end(suffix);
  require(end.size() < limits.max_name_bytes, "Clip name budget is too small");
  auto size = std::min(old.size(), limits.max_name_bytes - end.size());
  while (size < old.size() && size &&
         (static_cast<unsigned char>(old[size]) & 0xc0) == 0x80)
    --size;
  return old.substr(0, size) + end;
}
} // namespace
SourceIdentity source_identity(const Session &session, std::size_t prefix,
                               const Limits &limits) {
  prefix = sealed(session, prefix);
  return {1, prefix, hash_prefixes(session, {prefix}, limits).at(prefix)};
}
Binding bind(const Session &session, SceneKey scene, std::size_t prefix,
             const Limits &limits) {
  return FrameIndex(session, scene)
      .derive(source_identity(session, prefix, limits));
}
void validate_clip(const Session &session, const Clip &clip,
                   const Limits &limits) {
  verify_sources(session, {clip.binding.source}, limits);
  range_valid(session, clip, limits);
}
Catalog Catalog::create(const Session &session, SceneKey scene,
                        std::size_t prefix, Limits limits) {
  State state;
  state.binding = bind(session, scene, prefix, limits);
  state.clips.push_back(
      {1, "Scene range", state.binding, state.binding.available});
  state.selected = 1;
  state.next_id = 2;
  state_valid(session, state, limits);
  return Catalog(std::move(state), std::move(limits));
}
Catalog Catalog::restore(const Session &session, State state, Limits limits) {
  state_valid(session, state, limits);
  return Catalog(std::move(state), std::move(limits));
}
std::vector<CatalogRestoreResult>
restore_catalogs(const Session &session,const std::vector<State> &states,Limits limits) {
  limits_valid(limits);
  require(limits.max_catalogs&&states.size()<=limits.max_catalogs,
          "Clip restore catalog count exceeds the batch budget");
  std::vector<CatalogRestoreResult> results(states.size());
  std::vector<Verification> requested(states.size());
  std::map<SceneKey,FrameIndex> indices;
  std::set<std::size_t> prefixes,boundaries;
  for(std::size_t i=0;i<states.size();++i) {
    try {
      const auto scene=states[i].binding.scene;
      auto index=indices.find(scene);
      if(index==indices.end())
        index=indices.emplace(std::piecewise_construct,std::forward_as_tuple(scene),
            std::forward_as_tuple(session,scene)).first;
      requested[i]=state_structure(states[i],limits,index->second);
      for(const auto &source:requested[i].sources) {
        require(source.algorithm==1&&source.event_count<=session.event_count()&&
                    source.event_count<=std::numeric_limits<std::size_t>::max(),
                "Stored clip source prefix is invalid or unavailable");
        prefixes.insert(static_cast<std::size_t>(source.event_count));
      }
      boundaries.insert(requested[i].boundaries.begin(),requested[i].boundaries.end());
    }catch(const Error &e){results[i].error=e.what();requested[i]={};}
  }
  boundaries.insert(prefixes.begin(),prefixes.end());
  // Checkpoints run outside per-entry catches: a caller's cancellation must
  // cancel the entire restore, even when its exception type is clips::Error.
  const auto hashes=hash_prefixes(session,prefixes,limits);
  std::map<std::size_t,bool> safe;
  if(boundaries.count(0))safe[0]=true;
  if(!boundaries.empty()) {
    const auto total=*boundaries.rbegin();
    require(total<=session.event_count(),"Clip restore boundary exceeds received source");
    std::set<std::uint64_t> pending;
    for(std::size_t i=0;i<total;++i) {
      if(limits.checkpoint&&i%4096==0)limits.checkpoint(i,total);
      const auto &event=session.events()[i];
      if(std::holds_alternative<BeginSetPropertyValue>(event.payload)||
          std::holds_alternative<BeginPropertyMessageGroup>(event.payload))
        require(pending.insert(event.stream_id).second,"Clip source has nested property transactions");
      else if(std::holds_alternative<EndSetPropertyValue>(event.payload)||
              std::holds_alternative<EndPropertyMessageGroup>(event.payload))
        require(pending.erase(event.stream_id)!=0,"Clip source has unmatched transaction end");
      if(boundaries.count(i+1))safe[i+1]=pending.empty();
    }
  }
  for(std::size_t i=0;i<states.size();++i)if(results[i].error.empty()) {
    try {
      for(const auto &source:requested[i].sources)
        require(hashes.at(static_cast<std::size_t>(source.event_count))==source.fingerprint,
                "Clip source content differs from the stored sealed prefix");
      for(const auto boundary:requested[i].boundaries)
        require(safe.at(boundary),"Clip boundary is inside a property transaction");
      for(const auto &source:requested[i].sources)
        require(safe.at(static_cast<std::size_t>(source.event_count)),
                "Clip source seal is inside a property transaction");
      results[i].catalog=Catalog(states[i],limits);
    }catch(const Error &e){results[i].error=e.what();}
  }
  return results;
}
const Clip *Catalog::find(Id id) const noexcept {
  const auto found =
      std::find_if(state_.clips.begin(), state_.clips.end(),
                   [&](const auto &clip) { return clip.id == id; });
  return found == state_.clips.end() ? nullptr : &*found;
}
const Clip *Catalog::selected() const noexcept {
  return state_.selected ? find(*state_.selected) : nullptr;
}
void Catalog::validate(const Session &session) const {
  state_valid(session, state_, limits_);
}
void Catalog::refresh(const Session &session, std::size_t prefix) {
  validate(session);
  auto next = bind(session, state_.binding.scene, prefix, limits_);
  require(next.source.event_count >= state_.binding.source.event_count,
          "Refreshing a clip catalog cannot move its sealed prefix backward");
  state_.binding = std::move(next);
}
Id Catalog::add(const Session &session, std::string name, Span span) {
  validate(session);
  require(state_.clips.size() < limits_.max_clips &&
              state_.next_id != UINT64_MAX,
          "Clip count or stable ID budget is exhausted");
  Clip next{state_.next_id, std::move(name), state_.binding, span};
  range_valid(session, next, limits_);
  const auto id = next.id;
  state_.clips.push_back(std::move(next));
  ++state_.next_id;
  state_.selected = id;
  return id;
}
bool Catalog::select(const Session &session, Id id) {
  validate(session);
  if (!find(id))
    return false;
  state_.selected = id;
  return true;
}
bool Catalog::erase(const Session &session, Id id) {
  validate(session);
  const auto at = std::find_if(state_.clips.begin(), state_.clips.end(),
                               [&](const auto &clip) { return clip.id == id; });
  if (at == state_.clips.end())
    return false;
  const auto index = static_cast<std::size_t>(at - state_.clips.begin());
  state_.clips.erase(at);
  if (state_.selected == id) {
    state_.selected.reset();
    if (!state_.clips.empty())
      state_.selected =
          state_.clips[std::min(index, state_.clips.size() - 1)].id;
  }
  return true;
}
void Catalog::rename(const Session &session, Id id, std::string name) {
  validate(session);
  name_valid(name, limits_);
  auto at = std::find_if(state_.clips.begin(), state_.clips.end(),
                         [&](const auto &clip) { return clip.id == id; });
  require(at != state_.clips.end(), "Clip ID is absent");
  at->name.swap(name);
}
std::pair<Id, Id> Catalog::split(const Session &session, std::size_t playhead) {
  validate(session);
  const auto *source = selected();
  require(source && playhead >= source->frames.first &&
              playhead < source->frames.last,
          "Split needs a selected range and a playhead before its last frame");
  require(state_.clips.size() < limits_.max_clips &&
              state_.next_id < UINT64_MAX - 1,
          "Split exceeds the clip count or stable ID budget");
  auto left = *source, right = *source;
  left.id = state_.next_id;
  right.id = state_.next_id + 1;
  left.name = split_name(source->name, " (L)", limits_);
  right.name = split_name(source->name, " (R)", limits_);
  left.frames.last = playhead;
  right.frames.first = playhead + 1;
  range_valid(session, left, limits_);
  range_valid(session, right, limits_);
  auto next = state_.clips;
  const auto at = std::find_if(next.begin(), next.end(), [&](const auto &clip) {
    return clip.id == source->id;
  });
  const auto index = static_cast<std::size_t>(at - next.begin());
  next[index] = std::move(left);
  next.insert(next.begin() + static_cast<std::ptrdiff_t>(index + 1),
              std::move(right));
  state_.clips.swap(next);
  const auto ids = std::make_pair(state_.next_id, state_.next_id + 1);
  state_.next_id += 2;
  state_.selected = ids.first;
  return ids;
}
void Catalog::trim_left(const Session &session, std::size_t playhead) {
  validate(session);
  const auto *source = selected();
  require(source && playhead >= source->frames.first &&
              playhead <= source->frames.last,
          "Trim playhead is outside the selected native range");
  for (auto &clip : state_.clips)
    if (clip.id == source->id) {
      auto next = clip;
      next.frames.first = playhead;
      range_valid(session, next, limits_);
      clip = std::move(next);
      return;
    }
}
void Catalog::trim_right(const Session &session, std::size_t playhead) {
  validate(session);
  const auto *source = selected();
  require(source && playhead >= source->frames.first &&
              playhead <= source->frames.last,
          "Trim playhead is outside the selected native range");
  for (auto &clip : state_.clips)
    if (clip.id == source->id) {
      auto next = clip;
      next.frames.last = playhead;
      range_valid(session, next, limits_);
      clip = std::move(next);
      return;
    }
}
} // namespace pvd::clips
