#pragma once
// Public core API: SDK/UI-independent, source-bound native-frame clip values.
#include "pvd/session.hpp"
#include <functional>
#include <limits>
#include <tuple>

namespace pvd::clips {
using Id = std::uint64_t;
struct SourceIdentity {
  std::uint32_t algorithm = 1;
  std::uint64_t event_count = 0, fingerprint = 0;
  bool operator==(const SourceIdentity &r) const noexcept {
    return algorithm == r.algorithm && event_count == r.event_count &&
           fingerprint == r.fingerprint;
  }
};
struct SceneKey {
  std::uint64_t id = 0, generation = 0;
  bool operator==(const SceneKey &r) const noexcept {
    return id == r.id && generation == r.generation;
  }
  bool operator!=(const SceneKey &r) const noexcept { return !(*this == r); }
  bool operator<(const SceneKey &r) const noexcept {
    return std::tie(id, generation) < std::tie(r.id, r.generation);
  }
};
struct Span {
  // Inclusive zero-based ordinals in ALL native frames for this scene ID,
  // including earlier generations. These are the existing ExportJob indices.
  std::size_t first = 0, last = 0;
  std::size_t count() const noexcept {
    return last >= first ? last - first + 1 : 0;
  }
};
struct Binding {
  SourceIdentity source; // A sealed transaction-safe prefix, not a file path.
  SceneKey scene;
  Span available; // Closed frames in this exact scene lifetime at this prefix.
  std::size_t native_frame_count = 0; // All this scene ID's closed frames.
};
struct Clip {
  Id id = 0;
  std::string name;
  Binding binding; // Never silently extended by Catalog::refresh().
  Span frames;
};
struct State {
  Binding binding;
  std::vector<Clip> clips;
  std::optional<Id> selected;
  Id next_id = 1; // IDs are stable and are never reused after delete/split.
};
struct Limits {
  std::size_t max_clips = 1024, max_name_bytes = 256;
  std::size_t max_event_bytes = 64u * 1024u * 1024u;
  std::size_t max_catalogs = 64;
  // Runs between events and <=64 KiB hashing steps; exceptions cancel before
  // publishing a changed catalog. It is never called while drawing the list.
  std::function<void(std::size_t done, std::size_t total)> checkpoint;
};
class Error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
// Algorithm 1 is the workspace canonical FNV1a hash of header followed by each
// event's LE stream/timestamp and original-byte-order encoded payload. At EOF
// it equals workspace_source_identity(). This is identity checking, not
// security.
PVD_CORE_API SourceIdentity
source_identity(const Session &,
                std::size_t prefix = std::numeric_limits<std::size_t>::max(),
                const Limits & = {});
PVD_CORE_API Binding
bind(const Session &, SceneKey,
     std::size_t sealed_prefix = std::numeric_limits<std::size_t>::max(),
     const Limits & = {});
PVD_CORE_API void validate_clip(const Session &, const Clip &, const Limits & = {});

struct CatalogRestoreResult;
PVD_CORE_API std::vector<CatalogRestoreResult>
restore_catalogs(const Session &, const std::vector<State> &, Limits = {});

class Catalog {
public:
  // Creates one selected "Scene range" spanning the exact lifetime's closed
  // native frames. Live append is supported; an open frame is never an end.
  static PVD_CORE_API Catalog
  create(const Session &, SceneKey,
         std::size_t sealed_prefix = std::numeric_limits<std::size_t>::max(),
         Limits = {});
  static PVD_CORE_API Catalog restore(const Session &, State, Limits = {});
  const State &state() const noexcept { return state_; }
  PVD_CORE_API const Clip *find(Id) const noexcept;
  PVD_CORE_API const Clip *selected() const noexcept;
  PVD_CORE_API void validate(const Session &) const;
  // Explicitly admits newly closed frames. Existing Clip values, source seals
  // and selections remain unchanged. Replaced source contents are rejected.
  PVD_CORE_API void
  refresh(const Session &,
          std::size_t sealed_prefix = std::numeric_limits<std::size_t>::max());
  PVD_CORE_API Id add(const Session &, std::string name, Span);
  PVD_CORE_API bool select(const Session &, Id);
  PVD_CORE_API bool erase(const Session &, Id);
  PVD_CORE_API void rename(const Session &, Id, std::string name);
  // Split includes playhead in left; right starts at playhead+1. Empty sides
  // are rejected, the original ID is retired, and the left clip is selected.
  PVD_CORE_API std::pair<Id, Id> split(const Session &, std::size_t native_playhead);
  // Trims preserve the current original-frame ordinal and selected ID.
  PVD_CORE_API void trim_left(const Session &, std::size_t native_playhead);
  PVD_CORE_API void trim_right(const Session &, std::size_t native_playhead);

private:
  friend PVD_CORE_API std::vector<CatalogRestoreResult>
  restore_catalogs(const Session &, const std::vector<State> &, Limits);
  Catalog(State state, Limits limits)
      : state_(std::move(state)), limits_(std::move(limits)) {}
  State state_;
  Limits limits_;
};
struct CatalogRestoreResult {
  std::optional<Catalog> catalog;
  std::string error;
};
// One result per input, preserving order. Structural/source mismatches are
// per-entry errors; allocation failures and checkpoint exceptions propagate.
// Prefix hashing and transaction-boundary scans are shared across the batch.
} // namespace pvd::clips
