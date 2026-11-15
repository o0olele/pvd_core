#pragma once

#include "pvd/protocol.hpp"
#include <array>
#include <algorithm>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace pvd {

struct QualifiedName {
  std::string namespace_name, name;
  std::string str() const {
    return namespace_name.empty() ? name : namespace_name + "::" + name;
  }
  bool operator<(const QualifiedName &rhs) const {
    return std::tie(namespace_name, name) <
           std::tie(rhs.namespace_name, rhs.name);
  }
  bool operator==(const QualifiedName &rhs) const {
    return namespace_name == rhs.namespace_name && name == rhs.name;
  }
  bool operator!=(const QualifiedName &rhs) const { return !(*this == rhs); }
};
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
struct Transform {
  Quat rotation;
  Vec3 position;
}; // SDK layout: quaternion, then translation.

struct PropertyDefinition {
  std::string name, semantic;
  QualifiedName datatype;
  std::uint8_t property_type = 1; // SDK Unknown=0, Scalar=1, Array=2.
  std::map<std::string, std::uint32_t> named_values;
};
struct ClassDefinition {
  QualifiedName name;
  std::optional<QualifiedName> parent;
  std::map<std::string, PropertyDefinition> properties;
};
struct MessageArgument {
  std::string property_name;
  QualifiedName datatype;
  std::uint32_t offset = 0, byte_size = 0;
};
struct MessageDefinition {
  QualifiedName name, class_name;
  std::uint32_t byte_size = 0;
  std::vector<MessageArgument> arguments;
};
struct Metadata {
  std::map<QualifiedName, ClassDefinition> classes;
  std::map<QualifiedName, MessageDefinition> messages;
  PVD_CORE_API const ClassDefinition *
  find_class(const QualifiedName &) const noexcept;
  PVD_CORE_API const PropertyDefinition *
  find_property(const QualifiedName &, const std::string &) const noexcept;
  PVD_CORE_API bool is_a(const QualifiedName &derived,
                         const QualifiedName &base) const noexcept;
};

// Immutable payloads are shared by snapshots/checkpoints. raw_bytes() retains
// the incoming wire encoding, including source pointer width, not host pointer
// width.
struct PropertyValue {
  QualifiedName incoming_type;
  std::uint64_t item_count = 0;
  ByteOrder byte_order = ByteOrder::little;
  std::uint8_t pointer_width = 4;
  std::shared_ptr<const Bytes> bytes;
  std::optional<std::string> text;
  std::optional<std::vector<std::uint64_t>> reference_ids;
  bool opaque = false;
  // Explicit PhysX-native coordinate view. Original bytes and all as_* getters
  // remain wire-exact. A bounded shadow is updated with each individual float
  // OriginShift; a fresh write resets it, including same-byte cross-epoch writes.
  std::shared_ptr<const Bytes> native_coordinate_bytes;
  std::uint64_t coordinate_epoch = 0;
  PVD_CORE_API const Bytes &raw_bytes() const noexcept;
  PVD_CORE_API std::optional<std::uint64_t> as_uint(std::size_t item = 0) const;
  PVD_CORE_API std::optional<std::int64_t> as_int(std::size_t item = 0) const;
  PVD_CORE_API std::optional<float> as_float(std::size_t item = 0) const;
  PVD_CORE_API std::optional<double> as_double(std::size_t item = 0) const;
  PVD_CORE_API std::optional<Vec3> as_vec3(std::size_t item = 0) const;
  PVD_CORE_API std::optional<Quat> as_quat(std::size_t item = 0) const;
  PVD_CORE_API std::optional<Transform>
  as_transform(std::size_t item = 0) const;
  std::optional<std::string> as_string() const { return text; }
  PVD_CORE_API std::optional<std::uint64_t>
  as_object_ref(std::size_t item = 0) const;
};
struct InstanceState {
  std::uint64_t id = 0, generation = 0, creating_stream = 0;
  QualifiedName class_name;
  std::map<std::string, PropertyValue> properties;
  bool pickable = true, top_level = false;
  std::optional<Color> color;
  std::uint64_t coordinate_epoch = 0;
  PVD_CORE_API const PropertyValue *
  property(const std::string &name) const noexcept;
};
struct CameraState {
  Vec3 position, up, target;
};
struct Snapshot {
  std::size_t event_count = 0;
  // Only live instances are present; generation distinguishes reuse of an ID.
  std::map<std::uint64_t, InstanceState> instances;
  std::map<std::string, CameraState> cameras;
  // Explicit scene offsets; raw property bytes are never rewritten. Use
  // coordinates::native_* for retained native world-space values after shifts.
  std::map<std::uint64_t, Vec3> origin_shifts;
  PVD_CORE_API const InstanceState *instance(std::uint64_t id) const noexcept;
};
// Immutable map facade. Iteration/find/at expose the same object references as
// Snapshot, but no mutable alias can escape and invalidate a published view.
class SnapshotInstances {
public:
  class Storage {
    using Entry =
        std::pair<std::uint64_t, std::shared_ptr<const InstanceState>>;
    std::vector<Entry> entries_;

  public:
    using iterator = std::vector<Entry>::iterator;
    using const_iterator = std::vector<Entry>::const_iterator;
    iterator begin() noexcept { return entries_.begin(); }
    iterator end() noexcept { return entries_.end(); }
    const_iterator begin() const noexcept { return entries_.begin(); }
    const_iterator end() const noexcept { return entries_.end(); }
    iterator lower_bound(std::uint64_t id) {
      return std::lower_bound(begin(), end(), id,
                              [](const Entry &entry, std::uint64_t key) {
                                return entry.first < key;
                              });
    }
    const_iterator lower_bound(std::uint64_t id) const {
      return std::lower_bound(begin(), end(), id,
                              [](const Entry &entry, std::uint64_t key) {
                                return entry.first < key;
                              });
    }
    iterator find(std::uint64_t id) {
      auto found = lower_bound(id);
      return found != end() && found->first == id ? found : end();
    }
    const_iterator find(std::uint64_t id) const {
      auto found = lower_bound(id);
      return found != end() && found->first == id ? found : end();
    }
    std::size_t count(std::uint64_t id) const { return find(id) != end(); }
    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
    void reserve(std::size_t count) { entries_.reserve(count); }
    std::pair<iterator, bool>
    emplace(std::uint64_t id, std::shared_ptr<const InstanceState> object) {
      auto found = lower_bound(id);
      if (found != end() && found->first == id)
        return {found, false};
      return {entries_.emplace(found, id, std::move(object)), true};
    }
    std::shared_ptr<const InstanceState> &operator[](std::uint64_t id) {
      auto found = lower_bound(id);
      if (found == end() || found->first != id)
        found =
            entries_.emplace(found, id, std::shared_ptr<const InstanceState>{});
      return found->second;
    }
    const std::shared_ptr<const InstanceState> &at(std::uint64_t id) const {
      auto found = find(id);
      if (found == end())
        throw std::out_of_range("snapshot instance not found");
      return found->second;
    }
    std::size_t erase(std::uint64_t id) {
      auto found = find(id);
      if (found == end())
        return 0;
      entries_.erase(found);
      return 1;
    }
  };
  class const_iterator {
    Storage::const_iterator iterator_;

  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<const std::uint64_t, InstanceState>;
    using reference = std::pair<const std::uint64_t &, const InstanceState &>;
    struct pointer {
      reference value;
      const reference *operator->() const { return &value; }
    };
    const_iterator() = default;
    explicit const_iterator(Storage::const_iterator value) : iterator_(value) {}
    reference operator*() const {
      return {iterator_->first, *iterator_->second};
    }
    pointer operator->() const { return {operator*()}; }
    const_iterator &operator++() {
      ++iterator_;
      return *this;
    }
    const_iterator operator++(int) {
      auto old = *this;
      ++*this;
      return old;
    }
    const_iterator &operator--() {
      --iterator_;
      return *this;
    }
    const_iterator operator--(int) {
      auto old = *this;
      --*this;
      return old;
    }
    bool operator==(const const_iterator &other) const {
      return iterator_ == other.iterator_;
    }
    bool operator!=(const const_iterator &other) const {
      return !(*this == other);
    }
  };
  using iterator = const_iterator;
  PVD_CORE_API SnapshotInstances();
  SnapshotInstances(const SnapshotInstances &) = default;
  SnapshotInstances &operator=(const SnapshotInstances &) = default;
  SnapshotInstances(SnapshotInstances &&other) noexcept
      : storage_(other.storage_) {}
  SnapshotInstances &operator=(SnapshotInstances &&other) noexcept {
    storage_ = other.storage_;
    return *this;
  }
  const_iterator begin() const { return const_iterator(storage_->begin()); }
  const_iterator end() const { return const_iterator(storage_->end()); }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }
  const_iterator find(std::uint64_t id) const {
    return const_iterator(storage_->find(id));
  }
  const_iterator lower_bound(std::uint64_t id) const {
    return const_iterator(storage_->lower_bound(id));
  }
  std::size_t count(std::uint64_t id) const { return storage_->count(id); }
  std::size_t size() const noexcept { return storage_->size(); }
  bool empty() const noexcept { return storage_->empty(); }
  const InstanceState &at(std::uint64_t id) const { return *storage_->at(id); }

private:
  explicit SnapshotInstances(std::shared_ptr<const Storage> storage)
      : storage_(std::move(storage)) {}
  std::shared_ptr<const Storage> storage_;
  friend class Session;
  friend struct SnapshotView;
};
struct SnapshotView {
  std::size_t event_count = 0;
  SnapshotInstances instances;
  std::map<std::string, CameraState> cameras;
  std::map<std::uint64_t, Vec3> origin_shifts;
  SnapshotView() = default;
  PVD_CORE_API SnapshotView(Snapshot snapshot);
  PVD_CORE_API const InstanceState *instance(std::uint64_t id) const noexcept;
  PVD_CORE_API Snapshot materialize() const;
  PVD_CORE_API Snapshot select(const std::vector<std::uint64_t> &ids) const;
  PVD_CORE_API SnapshotView
  select_view(const std::vector<std::uint64_t> &ids) const;
  operator Snapshot() const { return materialize(); }
};
struct ObjectLifetime {
  std::uint64_t id = 0, generation = 0, creating_stream = 0;
  QualifiedName class_name;
  std::size_t created_event = 0;
  std::optional<std::size_t> destroyed_event;
};
struct SectionMarker {
  std::uint64_t stream_id = 0, section_id = 0;
  std::string name;
  std::size_t begin_event_count = 0, event_count = 0;
  std::uint64_t begin_timestamp = 0, end_timestamp = 0;
};
struct FrameMarker {
  std::uint64_t stream_id = 0, scene_id = 0;
  std::size_t index = 0; // Zero-based within (stream_id, scene_id).
  std::size_t begin_event_count = 0,
              event_count = 0; // Snapshot after first event_count events.
  std::uint64_t begin_timestamp = 0, end_timestamp = 0;
};
struct ChannelPacket {
  std::size_t event_index = 0;
  std::uint64_t stream_id = 0, instance_id = 0;
  std::string property_name, semantic;
  PropertyValue value;
};
struct ProfileZoneInfo {
  std::uint64_t id = 0;
  std::string name;
  std::map<std::uint16_t, AddProfileZoneEvent> events;
};
struct SessionOptions {
  std::size_t checkpoint_interval = 4096;
  std::size_t max_checkpoints =
      64; // Limits state copies; zero disables checkpoints.
  DecodeLimits decode_limits;
  // Estimated owned checkpoint nodes/strings/reference vectors. Immutable
  // property byte buffers are shared and are not counted again. Zero disables
  // checkpoints; SIZE_MAX removes this byte bound (the count bound remains).
  std::size_t max_checkpoint_bytes = 128u * 1024u * 1024u;
  // Owned instance maps and derived reference versions retained by read queries.
  // Already published views own their objects independently of cache eviction.
  // Zero disables strong retention; weak reuse of view-held objects remains.
  std::size_t max_view_cache_bytes = 128u * 1024u * 1024u;
};
class SessionError : public std::runtime_error {
public:
  PVD_CORE_API SessionError(std::string message, std::size_t event_index,
                            std::uint64_t byte_offset);
  std::size_t event_index() const noexcept { return event_index_; }
  std::uint64_t byte_offset() const noexcept { return byte_offset_; }

private:
  std::size_t event_index_;
  std::uint64_t byte_offset_;
};

// Only thrown by state/index queries if allocation failure also prevented
// rebuilding the accepted prefix. header(), events() and event_count() remain
// available, allowing the caller to retry reconstruction after memory recovers.
class SessionRecoveryError : public std::exception {
public:
  const char *what() const noexcept override {
    return "session state unavailable: failed to recover accepted event prefix";
  }
};

class Session {
public:
  // Live session: append only complete decoded events. External synchronization
  // is required between append/finish and all reference-returning queries.
  // Do not retain event/index element references across append. Snapshots own
  // their state and share immutable property bytes, so existing snapshots
  // survive. Metadata/index references must also be released before append,
  // including append that fails and rebuilds the accepted prefix.
  PVD_CORE_API explicit Session(StreamHeader header,
                                SessionOptions options = {});
  PVD_CORE_API Session(StreamHeader header, std::vector<Event> events,
                       SessionOptions options = {});
  PVD_CORE_API ~Session();
  PVD_CORE_API Session(Session &&) noexcept;
  PVD_CORE_API Session &operator=(Session &&) noexcept;
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  static PVD_CORE_API Session parse(const Bytes &, SessionOptions options = {});
  // File paths are UTF-8, including on Windows.
  static PVD_CORE_API Session load_file(const std::string &path,
                                        SessionOptions options = {});
  // A rejected event is excluded from events()/event_count(). Earlier events in
  // the same batch remain committed and queryable. Failure prohibits append.
  // If persistent allocation failure prevents recovery, state_available() is
  // false and state/index queries throw SessionRecoveryError instead of
  // exposing partial state. The accepted event source remains accessible.
  PVD_CORE_API void append(std::vector<Event> events);
  PVD_CORE_API void finish();
  PVD_CORE_API std::size_t event_count() const noexcept;
  PVD_CORE_API bool finished() const noexcept;
  PVD_CORE_API bool failed() const noexcept;
  PVD_CORE_API bool state_available() const noexcept;
  // Open ordinary sections do not prevent a recoverable property boundary.
  PVD_CORE_API bool at_transaction_boundary() const noexcept;
  PVD_CORE_API const StreamHeader &header() const noexcept;
  PVD_CORE_API const Metadata &metadata() const;
  PVD_CORE_API const std::vector<Event> &events() const noexcept;
  PVD_CORE_API const std::vector<FrameMarker> &frames() const;
  PVD_CORE_API const std::vector<SectionMarker> &sections() const;
  PVD_CORE_API const std::vector<ObjectLifetime> &lifetimes() const;
  PVD_CORE_API const std::vector<ChannelPacket> &channels() const;
  PVD_CORE_API const std::map<std::uint64_t, ProfileZoneInfo> &
  profile_zones() const;
  // Replays a global event prefix; querying an earlier prefix is reverse seek.
  PVD_CORE_API Snapshot snapshot_at_event(std::size_t event_count) const;
  // Exact projection at the requested prefix. Only requested live IDs are
  // present (references may target omitted objects). No inferred closure.
  // Native coordinate state remains exact; origin-shift prefixes currently
  // use full-state replay internally before projection.
  PVD_CORE_API Snapshot snapshot_selected_at_event(
      std::size_t event_count, const std::vector<std::uint64_t> &ids) const;
  PVD_CORE_API std::optional<InstanceState>
  instance_at_event(std::size_t event_count, std::uint64_t id) const;
  // Cache-assisted read queries serialize their caches internally. Append and
  // finish still require the same external synchronization as other queries.
  PVD_CORE_API SnapshotView snapshot_view_at_event(std::size_t event_count) const;
  PVD_CORE_API SnapshotView snapshot_view_at_frame(std::size_t global_frame_index) const;
  PVD_CORE_API Snapshot snapshot_at_frame(std::size_t global_frame_index) const;
  PVD_CORE_API Snapshot snapshot_at_frame(std::uint64_t stream_id,
                                          std::uint64_t scene_id,
                                          std::size_t frame_index) const;
  PVD_CORE_API Snapshot latest() const;
  PVD_CORE_API std::size_t checkpoint_count() const;
  // Estimated owned structures retained by noninitial checkpoints; excludes
  // immutable shared byte buffers and allocator-specific overhead.
  PVD_CORE_API std::size_t estimated_checkpoint_bytes() const;
  PVD_CORE_API std::size_t estimated_view_cache_bytes() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pvd
