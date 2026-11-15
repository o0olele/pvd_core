#include "pvd/debug_render.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <set>

namespace pvd::render {
namespace {
struct Problem {
  DebugCode code;
  std::string message;
  std::size_t offset;
};
class Reader {
public:
  Reader(const Bytes &bytes, ByteOrder order) : bytes_(bytes), order_(order) {}
  std::size_t position() const { return pos_; }
  std::size_t remaining() const { return bytes_.size() - pos_; }
  void require(std::size_t size) const {
    if (size > remaining())
      throw Problem{DebugCode::malformed, "truncated debug payload", pos_};
  }
  void skip(std::size_t size) {
    require(size);
    pos_ += size;
  }
  std::uint8_t u8() {
    require(1);
    return bytes_[pos_++];
  }
  std::uint64_t integer(std::size_t width) {
    require(width);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
      const auto at = order_ == ByteOrder::little ? i : width - i - 1;
      value |= std::uint64_t(bytes_[pos_ + at]) << (i * 8);
    }
    pos_ += width;
    return value;
  }
  std::uint32_t u32() { return static_cast<std::uint32_t>(integer(4)); }
  float scalar() {
    auto bits = u32();
    float value;
    std::memcpy(&value, &bits, 4);
    if (!std::isfinite(value))
      throw Problem{DebugCode::malformed, "non-finite debug coordinate",
                    pos_ - 4};
    return value;
  }
  Vec3 vec() { return {scalar(), scalar(), scalar()}; }
  Transform transform() {
    Transform t;
    t.rotation = {scalar(), scalar(), scalar(), scalar()};
    t.position = vec();
    return t;
  }
  std::uint32_t count(std::size_t stride, std::size_t limit) {
    const auto at = pos_;
    auto n = u32();
    if (n > limit)
      throw Problem{DebugCode::limit_exceeded,
                    "debug primitive count exceeds limit", at};
    if (n > remaining() / stride)
      throw Problem{DebugCode::malformed,
                    "debug primitive array exceeds packet bounds", at};
    return n;
  }
  std::string text(std::size_t limit) {
    const auto at = pos_;
    const auto n = u32();
    if (n > limit)
      throw Problem{DebugCode::limit_exceeded,
                    "debug text length exceeds limit", at};
    if (!n)
      throw Problem{DebugCode::malformed,
                    "debug text must include a NUL terminator", at};
    require(n);
    if (bytes_[pos_ + n - 1])
      throw Problem{DebugCode::malformed, "unterminated debug text",
                    pos_ + n - 1};
    if (std::find(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_),
                  bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + n - 1),
                  0) !=
        bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + n - 1))
      throw Problem{DebugCode::malformed, "embedded NUL in debug text", pos_};
    std::string s(reinterpret_cast<const char *>(bytes_.data() + pos_), n - 1);
    pos_ += n;
    return s;
  }

private:
  const Bytes &bytes_;
  ByteOrder order_;
  std::size_t pos_ = 0;
};
void diagnostic(DebugResult &out, DebugCode code, std::string message,
                std::size_t event = 0, std::size_t offset = 0,
                std::uint64_t channel = 0) {
  out.complete = false;
  out.diagnostics.push_back({code, std::move(message), event, offset, channel});
}
std::size_t primitive_count(const DebugResult &out) {
  return out.points.size() + out.lines.size() + out.triangles.size() +
         out.texts.size() + out.constraints.size();
}
template <class SnapshotLike>
std::optional<bool> belongs(const SnapshotLike &snapshot, const Metadata &meta,
                            std::uint64_t id, std::uint64_t scene,
                            std::set<std::uint64_t> &seen) {
  if (id == scene)
    return true;
  if (!id || !seen.insert(id).second || seen.size() > 32)
    return {};
  const auto *object = snapshot.instance(id);
  if (!object)
    return {};
  if (meta.is_a(object->class_name, {"physx3", "PxScene"}))
    return false;
  bool known = false;
  for (const char *key :
       {"Scene", "Actor", "Actors", "Actors[0]", "Actors[1]",
        "Actors.actor0", "Actors.actor1"}) {
    const auto *p = object->property(key);
    if (!p || !p->reference_ids)
      continue;
    for (auto ref : *p->reference_ids) {
      auto match = belongs(snapshot, meta, ref, scene, seen);
      if (match && *match)
        return true;
      if (match)
        known = true;
    }
  }
  return known ? std::optional<bool>(false) : std::nullopt;
}
template <class SnapshotLike>
bool selected_scene(const SnapshotLike &snapshot, const Metadata &meta,
                    const DebugRange &range, const DebugSource &source) {
  if (!range.scene_id)
    return true;
  std::set<std::uint64_t> seen;
  auto association =
      belongs(snapshot, meta, source.instance_id, *range.scene_id, seen);
  if (association)
    return *association;
  return std::find(
             range.scene_renderer_ids.begin(), range.scene_renderer_ids.end(),
             source.channel_instance_id) != range.scene_renderer_ids.end();
}
void points(Reader &r, DebugResult &out, DebugSource source,
            const DebugOptions &options) {
  const auto n = r.count(16, options.max_primitives);
  out.points.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    auto p = r.vec();
    auto c = r.u32();
    out.points.push_back({source, p, c});
  }
}
void lines(Reader &r, DebugResult &out, DebugSource source,
           const DebugOptions &options) {
  const auto n = r.count(32, options.max_primitives);
  out.lines.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    auto p = r.vec();
    auto c = r.u32();
    auto q = r.vec();
    auto d = r.u32();
    out.lines.push_back({source, p, q, c, d});
  }
}
void triangles(Reader &r, DebugResult &out, DebugSource source,
               const DebugOptions &options) {
  const auto n = r.count(48, options.max_primitives);
  out.triangles.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    auto p = r.vec();
    auto c = r.u32();
    auto q = r.vec();
    auto d = r.u32();
    auto s = r.vec();
    auto e = r.u32();
    out.triangles.push_back({source, p, q, s, c, d, e});
  }
}
template <class T> void move_all(std::vector<T> &to, std::vector<T> &from) {
  to.insert(to.end(), std::make_move_iterator(from.begin()),
            std::make_move_iterator(from.end()));
}
Vec3 rotate(Quat q, Vec3 v) {
  Vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z),
         2 * (q.x * v.y - q.y * v.x)};
  return {v.x + q.w * t.x + q.y * t.z - q.z * t.y,
          v.y + q.w * t.y + q.z * t.x - q.x * t.z,
          v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}
void axes(DebugResult &out, DebugSource source, const Transform &t,
          float length) {
  const Vec3 directions[] = {{length, 0, 0}, {0, length, 0}, {0, 0, length}};
  const std::uint32_t colors[] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu};
  for (std::size_t i = 0; i < 3; ++i) {
    auto d = rotate(t.rotation, directions[i]);
    out.lines.push_back(
        {source,
         t.position,
         {t.position.x + d.x, t.position.y + d.y, t.position.z + d.z},
         colors[i],
         colors[i],
         true});
  }
}
} // namespace

namespace {
template <class SnapshotLike>
DebugResult extract_contacts_impl(const SnapshotLike &snapshot, const Metadata &metadata,
                             std::uint64_t scene_id, DebugOptions options) {
  DebugResult out;
  const auto *scene = snapshot.instance(scene_id);
  if (!scene || !metadata.is_a(scene->class_name, {"physx3", "PxScene"})) {
    diagnostic(out, DebugCode::missing_scene,
               "requested PxScene is not alive in this snapshot");
    return out;
  }
  const auto *p = scene->property("Contacts");
  if (!p)
    return out;
  if (p->incoming_type != QualifiedName{"physx3", "PvdContact"} ||
      (p->pointer_width != 4 && p->pointer_width != 8)) {
    diagnostic(
        out, DebugCode::unsupported_contacts,
        "Contacts does not use the verified PhysX 3.4 PvdContact layout");
    return out;
  }
  const auto stride = p->pointer_width == 8 ? std::size_t{64} : std::size_t{52};
  const auto &raw = p->raw_bytes();
  if (raw.size() % stride || p->item_count != raw.size() / stride) {
    diagnostic(out, DebugCode::malformed,
               "PvdContact count/byte length does not match source ABI stride");
    return out;
  }
  if (p->item_count > options.max_contacts) {
    diagnostic(out, DebugCode::limit_exceeded, "contact count exceeds limit");
    return out;
  }
  Reader r(raw, p->byte_order);
  try {
    std::vector<DebugContact> staged;
    staged.reserve(static_cast<std::size_t>(p->item_count));
    for (std::size_t i = 0; i < p->item_count; ++i) {
      const auto start = r.position();
      DebugContact c;
      c.scene_id = scene_id;
      c.position = r.vec();
      c.normal = r.vec();
      c.shape0 = r.integer(p->pointer_width);
      c.shape1 = r.integer(p->pointer_width);
      c.separation = r.scalar();
      const auto force_bits = r.u32();
      float force;
      std::memcpy(&force, &force_bits, 4);
      c.internal_face_index0 = r.u32();
      c.internal_face_index1 = r.u32();
      const bool available = r.u8() != 0;
      if (available) {
        if (!std::isfinite(force))
          throw Problem{DebugCode::malformed,
                        "non-finite available contact force",
                        start + 24 + 2 * p->pointer_width + 4};
        c.normal_force = force;
      }
      r.skip(stride - (r.position() - start));
      staged.push_back(c);
    }
    out.contacts = std::move(staged);
    out.contacts_available = true;
  } catch (const Problem &pbl) {
    diagnostic(out, pbl.code, pbl.message, 0, pbl.offset);
  }
  return out;
}

template <class SnapshotLike>
DebugResult extract_debug_impl(const SnapshotLike &snapshot, const Metadata &metadata,
                          const std::vector<ChannelPacket> &packets,
                          DebugRange range, DebugOptions options) {
  DebugResult out;
  if (range.begin_event_count > range.end_event_count ||
      !std::isfinite(options.joint_axis_length) ||
      options.joint_axis_length < 0) {
    diagnostic(out, DebugCode::invalid_range,
               "invalid debug event interval or joint axis length");
    return out;
  }
  if (range.scene_id)
    out = extract_contacts_impl(snapshot, metadata, *range.scene_id, options);
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> instance_ids;
  std::size_t previous = 0;
  bool have_previous = false;
  for (const auto &packet : packets) {
    if (have_previous && packet.event_index < previous) {
      diagnostic(out, DebugCode::invalid_range,
                 "debug packets must be ordered by event_index",
                 packet.event_index);
      return out;
    }
    previous = packet.event_index;
    have_previous = true;
    if (packet.event_index >= range.end_event_count)
      break;
    if (packet.semantic != "render event stream")
      continue;
    auto &instance = instance_ids[{packet.stream_id, packet.instance_id}];
    const auto &raw = packet.value.raw_bytes();
    if (raw.size() > options.max_packet_bytes) {
      diagnostic(out, DebugCode::limit_exceeded,
                 "render packet exceeds byte limit", packet.event_index, 0,
                 packet.instance_id);
      instance = 0;
      continue;
    }
    Reader r(raw, packet.value.byte_order);
    std::size_t event_count = 0;
    try {
      while (r.remaining()) {
        const auto offset = r.position();
        if (++event_count > options.max_events_per_packet)
          throw Problem{DebugCode::limit_exceeded,
                        "render event count exceeds packet limit", offset};
        const auto tag = r.u8();
        if (tag == 1) {
          instance = r.integer(8);
          continue;
        }
        if (packet.event_index < range.begin_event_count) {
          // Only SetInstanceId state survives old frames. Skip bulk arrays in
          // place so historical primitives consume neither output budgets nor
          // temporary payload allocations.
          auto skip_bulk = [&](std::size_t stride) {
            const auto count =
                r.count(stride, std::numeric_limits<std::uint32_t>::max());
            r.skip(static_cast<std::size_t>(count) * stride);
          };
          switch (tag) {
          case 2:
            skip_bulk(16);
            break;
          case 3:
            skip_bulk(32);
            break;
          case 4:
            skip_bulk(48);
            break;
          case 5:
            r.skip(56);
            break;
          case 6:
            r.skip(61);
            break;
          case 7:
          case 8:
            r.skip(37);
            break;
          case 9:
            r.skip(33);
            break;
          case 10:
            r.skip(20);
            (void)r.text(options.max_packet_bytes);
            break;
          case 11:
            skip_bulk(16);
            skip_bulk(32);
            skip_bulk(48);
            break;
          default:
            throw Problem{DebugCode::unsupported_event,
                          "unknown historical render tag cannot be skipped",
                          offset};
          }
          continue;
        }
        DebugSource source{packet.stream_id, packet.instance_id, instance,
                           packet.event_index, offset};
        DebugResult staged;
        switch (tag) {
        case 2:
          points(r, staged, source, options);
          break;
        case 3:
          lines(r, staged, source, options);
          break;
        case 4:
          triangles(r, staged, source, options);
          break;
        case 5: {
          DebugJointFrames c{r.transform(), r.transform()};
          staged.constraints.push_back({source, c});
          if (options.joint_axis_length > 0) {
            axes(staged, source, c.parent, options.joint_axis_length);
            axes(staged, source, c.child, options.joint_axis_length);
          }
          break;
        }
        case 6:
          staged.constraints.push_back(
              {source, DebugLinearLimit{r.transform(), r.transform(),
                                        r.scalar(), r.u8() != 0}});
          break;
        case 7:
          staged.constraints.push_back(
              {source, DebugAngularLimit{r.transform(), r.scalar(), r.scalar(),
                                         r.u8() != 0}});
          break;
        case 8:
          staged.constraints.push_back(
              {source, DebugLimitCone{r.transform(), r.scalar(), r.scalar(),
                                      r.u8() != 0}});
          break;
        case 9:
          staged.constraints.push_back(
              {source,
               DebugDoubleCone{r.transform(), r.scalar(), r.u8() != 0}});
          break;
        case 10: {
          DebugText t;
          t.source = source;
          t.color = r.u32();
          t.position = r.vec();
          t.size = r.scalar();
          t.text = r.text(options.max_text_bytes);
          staged.texts.push_back(std::move(t));
          break;
        }
        case 11:
          points(r, staged, source, options);
          lines(r, staged, source, options);
          triangles(r, staged, source, options);
          break;
        default:
          throw Problem{DebugCode::unsupported_event,
                        "unknown render tag " + std::to_string(tag) +
                            " cannot be skipped without a payload length",
                        offset};
        }
        if (primitive_count(staged) == 0)
          continue; // Empty SDK render buffers need no scene attribution.
        if (!selected_scene(snapshot, metadata, range, source)) {
          std::set<std::uint64_t> seen;
          const auto association =
              range.scene_id ? belongs(snapshot, metadata, source.instance_id,
                                       *range.scene_id, seen)
                             : std::optional<bool>(true);
          if (!association)
            diagnostic(out, DebugCode::unattributed_scene,
                       "render instance/channel has no established association "
                       "with requested scene",
                       packet.event_index, offset, packet.instance_id);
          continue;
        }
        const auto count = primitive_count(staged), old = primitive_count(out);
        if (old > options.max_primitives ||
            count > options.max_primitives - old)
          throw Problem{DebugCode::limit_exceeded,
                        "selected debug primitives exceed limit", offset};
        move_all(out.points, staged.points);
        move_all(out.lines, staged.lines);
        move_all(out.triangles, staged.triangles);
        move_all(out.texts, staged.texts);
        if (!staged.constraints.empty() &&
            (tag != 5 || options.joint_axis_length == 0))
          out.diagnostics.push_back(
              {DebugCode::constraint_not_tessellated,
               "constraint retained as exact typed data; no display "
               "scale/tessellation was invented",
               packet.event_index, offset, packet.instance_id});
        move_all(out.constraints, staged.constraints);
      }
    } catch (const Problem &pbl) {
      diagnostic(out, pbl.code, pbl.message, packet.event_index, pbl.offset,
                 packet.instance_id);
      instance = 0;
    }
  }
  return out;
}

} // namespace
DebugResult extract_contacts(const Snapshot &snapshot, const Metadata &metadata,
                             std::uint64_t scene_id, DebugOptions options) {
  return extract_contacts_impl(snapshot, metadata, scene_id, options);
}
DebugResult extract_contacts(const SnapshotView &snapshot, const Metadata &metadata,
                             std::uint64_t scene_id, DebugOptions options) {
  return extract_contacts_impl(snapshot, metadata, scene_id, options);
}
DebugResult extract_debug(const Snapshot &snapshot, const Metadata &metadata,
                          const std::vector<ChannelPacket> &packets,
                          DebugRange range, DebugOptions options) {
  return extract_debug_impl(snapshot, metadata, packets, std::move(range), options);
}
DebugResult extract_debug(const SnapshotView &snapshot, const Metadata &metadata,
                          const std::vector<ChannelPacket> &packets,
                          DebugRange range, DebugOptions options) {
  return extract_debug_impl(snapshot, metadata, packets, std::move(range), options);
}
DebugResult extract_debug(const Session &session, const FrameMarker &frame,
                          DebugOptions options,
                          std::vector<std::uint64_t> scene_renderer_ids) {
  const auto snapshot = session.snapshot_view_at_event(frame.event_count);
  return extract_debug(snapshot, session.metadata(), session.channels(),
                       {frame.begin_event_count, frame.event_count,
                        frame.scene_id, std::move(scene_renderer_ids)},
                       options);
}
} // namespace pvd::render
