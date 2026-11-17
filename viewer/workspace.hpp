#pragma once
#include "model.hpp"
#include "pvd/clips.hpp"
#include "view_preferences.hpp"
#include <functional>
#include <iosfwd>
#include <limits>

namespace pvd::viewer {
using SlotId = std::uint64_t;
using BookmarkId = std::uint64_t;
inline constexpr std::uint32_t workspace_format_version = 6;
enum class SlotLock { loading, live, exporting };
struct SourceIdentity {
  std::uint32_t algorithm =
      1; // Stable canonical FNV-1a 64; not a security hash.
  std::uint64_t event_count = 0, fingerprint = 0;
  bool operator==(const SourceIdentity &b) const noexcept {
    return algorithm == b.algorithm && event_count == b.event_count &&
           fingerprint == b.fingerprint;
  }
  bool operator!=(const SourceIdentity &b) const noexcept {
    return !(*this == b);
  }
};
// Can be computed in the host's loading future, then passed to add/replace.
SourceIdentity workspace_source_identity(const Session &);
// Called between bounded encoding/hash steps; exceptions propagate, allowing a
// background loader to cancel and report actual fingerprint event progress.
SourceIdentity workspace_source_identity(
    const Session &,
    const std::function<void(std::size_t done, std::size_t total)> &checkpoint);
const char *slot_lock_name(SlotLock) noexcept;
struct Bookmark {
  BookmarkId id = 0;
  std::string name;
  std::uint64_t scene_id = 0, frame_index = 0;
  SamplePoint sample_point = SamplePoint::native_frame;
  std::optional<OrbitCamera> camera;
  std::optional<SourceIdentity> identity;
};
struct StoredView {
  std::optional<std::uint64_t> scene_id;
  SamplePoint sample_point = SamplePoint::native_frame;
  std::uint64_t frame_index = 0;
  OrbitCamera camera;
  ViewOptions options;
  ViewPreferences preferences;
};
struct CaptureSettings {
  std::string source_path;
  std::vector<Bookmark> bookmarks;
  std::optional<StoredView> last_view;
  std::optional<SourceIdentity> identity;
  // Includes unverified source versions; never discard these on a failed load.
  std::vector<clips::State> clip_states;
};
struct SlotSettings {
  SlotId id = 0;
  std::string title, source_path;
  StoredView view;
  std::optional<SourceIdentity> identity;
};
struct WorkspaceSettings {
  std::uint32_t version = workspace_format_version;
  std::optional<SlotId> active_slot;
  std::vector<SlotSettings> slots;
  std::vector<CaptureSettings> captures;
  std::vector<std::string> recent_files;
  ViewPreferences default_preferences;
  ViewOptions default_view_options;
};
struct WorkspaceLimits {
  std::size_t max_slots = 32, max_captures = 256, max_recent_files = 32;
  std::size_t max_bookmarks_per_capture = 1024;
  std::size_t max_string_bytes = 1024u * 1024u;
  std::size_t max_file_bytes = 16u * 1024u * 1024u;
  std::size_t max_clip_catalogs_per_capture = 64;
  std::size_t max_clips_per_catalog = 1024, max_clips_per_capture = 4096;
  std::size_t max_clip_name_bytes = 256;
};
class WorkspaceError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
// App-owned versioned binary sidecar; all text is validated UTF-8. Fixed-width
// integers are exact, no raw capture bytes are embedded. No automatic I/O.
Bytes encode_workspace(const WorkspaceSettings &, const WorkspaceLimits & = {});
WorkspaceSettings decode_workspace(const Bytes &, const WorkspaceLimits & = {});
WorkspaceSettings load_workspace_file(const std::string &utf8_path,
                                      const WorkspaceLimits & = {});
void save_workspace_file(const std::string &utf8_path,
                         const WorkspaceSettings &, bool overwrite = false,
                         const WorkspaceLimits & = {});
class Workspace;
class WorkspaceSlot {
public:
  ~WorkspaceSlot();
  SlotId id() const noexcept;
  const std::string &title() const noexcept;
  const std::string &source_path() const noexcept;
  Document &document() noexcept;
  const Document &document() const noexcept;
  OrbitCamera &camera() noexcept;
  const OrbitCamera &camera() const noexcept;
  ViewPreferences &preferences() noexcept;
  const ViewPreferences &preferences() const noexcept;
  bool camera_ready() const noexcept;
  void set_camera_ready(bool value = true) noexcept;
  std::optional<SlotLock> lock_reason() const noexcept;
  const std::vector<Bookmark> &bookmarks() const noexcept;
  bool bookmarks_verified() const noexcept;
  bool bookmark_verified(BookmarkId) const noexcept;
  std::optional<SourceIdentity> source_identity() const noexcept;
  // Cached on commands/load/bind, never rehash on a getter/draw. Active values
  // passed sealed-prefix/scene-generation validation against this Session.
  const std::vector<clips::State> &clip_states() const noexcept;
  const std::vector<clips::State> &pending_clip_states() const noexcept;
  const std::map<clips::SceneKey, clips::Catalog> &
  clip_catalogs() const noexcept;
  std::uint64_t clip_revision() const noexcept;
  const std::string &warning() const noexcept;

private:
  friend class Workspace;
  struct Impl;
  explicit WorkspaceSlot(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
struct WorkspaceRestore {
  // Placeholders only: host may asynchronously load each source into this ID.
  std::vector<SlotId> source_slots;
  std::vector<std::string> warnings;
};
// Single owner/UI thread, no receiver or background worker is owned here.
// Slots and their Documents are heap stable across add/activate/reordering.
// lock() is exclusive and prevents replace/close/whole-workspace restore.
// A live receiver stays bound to its original SlotId while another is active;
// only that owner may mutate the live Document. Export locks require immutable
// Session access until the host releases the lock after its worker stops.
class Workspace {
public:
  explicit Workspace(ModelOptions = {}, WorkspaceLimits = {});
  ~Workspace();
  Workspace(const Workspace &) = delete;
  Workspace &operator=(const Workspace &) = delete;
  SlotId create_empty(std::string title = "Untitled");
  SlotId add(std::unique_ptr<Document>, std::string source_path = {},
             std::string title = {},
             std::optional<SourceIdentity> precomputed_identity = {});
  // Moves incoming only on success; false means missing/locked target. A saved
  // view unavailable in a changed capture is reported in warnings, not guessed.
  bool replace(SlotId, std::unique_ptr<Document> &incoming,
               std::string source_path = {},
               std::vector<std::string> *warnings = nullptr,
               std::optional<SourceIdentity> precomputed_identity = {});
  // Associate a stopped live/unsaved Document with its completed raw file,
  // preserving the Document/Session address, current view and valid bookmarks.
  bool bind_source(SlotId, std::string source_path,
                   std::optional<SourceIdentity> precomputed_identity = {},
                   std::vector<std::string> *warnings = nullptr);
  bool close(SlotId);
  bool activate(SlotId) noexcept;
  bool rename(SlotId, std::string title);
  bool lock(SlotId, SlotLock);
  bool unlock(SlotId, SlotLock) noexcept;
  bool any_locked() const noexcept;
  WorkspaceSlot *find(SlotId) noexcept;
  const WorkspaceSlot *find(SlotId) const noexcept;
  WorkspaceSlot *active() noexcept;
  const WorkspaceSlot *active() const noexcept;
  std::optional<SlotId> active_id() const noexcept;
  std::vector<WorkspaceSlot *> slots();
  std::vector<const WorkspaceSlot *> slots() const;
  const std::vector<std::string> &recent_files() const noexcept;
  const ViewPreferences &default_preferences() const noexcept;
  // Applies only to future empty/source slots without a verified saved view.
  // Invalid values throw before changing defaults or any existing slot.
  void set_default_preferences(ViewPreferences);
  const ViewOptions &default_view_options() const noexcept;
  void set_default_view_options(ViewOptions);
  void remember_recent(std::string utf8_path);
  BookmarkId add_bookmark(SlotId, std::string name, bool include_camera = true);
  bool remove_bookmark(SlotId, BookmarkId);
  bool rename_bookmark(SlotId, BookmarkId, std::string name);
  bool jump_to_bookmark(SlotId, BookmarkId);
  // Replaces this Session's verified catalogs while preserving unverified
  // foreign-source metadata. Loaded inputs are strictly validated; placeholders
  // store structurally valid values as pending. Ordinary live locks are
  // allowed.
  bool set_clip_states(SlotId, std::vector<clips::State>,
                       std::vector<std::string> *warnings = nullptr);
  // Keeps both sets. Conflicting stable clip IDs are reassigned, never dropped.
  bool merge_clip_states(SlotId, std::vector<clips::State>,
                         std::vector<std::string> *warnings = nullptr);
  bool verify_clip_states(SlotId, std::vector<std::string> *warnings = nullptr);
  // Explicit user command; ordinary setter/load never drops pending states.
  bool discard_pending_clip_states(SlotId);
  WorkspaceSettings settings() const;
  WorkspaceRestore restore(const WorkspaceSettings &);
  void save_file(const std::string &utf8_path, bool overwrite = false) const;
  WorkspaceRestore load_file(const std::string &utf8_path);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace pvd::viewer
