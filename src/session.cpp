#include "pvd/session.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>

namespace pvd {
namespace {
std::optional<std::size_t> builtin_size(const QualifiedName &t,
                                        std::uint8_t p) {
  if (t.namespace_name != "physx3")
    return {};
  const auto &n = t.name;
  if (n == "PvdI8" || n == "PvdU8" || n == "PvdBool")
    return 1;
  if (n == "PvdI16" || n == "PvdU16")
    return 2;
  if (n == "PvdI32" || n == "PvdU32" || n == "PvdF32" || n == "StringHandle" ||
      n == "ObjectRef" || n == "PvdColor")
    return 4;
  if (n == "PvdI64" || n == "PvdU64" || n == "PvdF64" || n == "PxVec2")
    return 8;
  if (n == "String" || n == "VoidPtr")
    return p;
  if (n == "PxVec3")
    return 12;
  if (n == "PxVec4" || n == "PxQuat" || n == "U32Array4")
    return 16;
  if (n == "PxBounds3")
    return 24;
  if (n == "PxTransform")
    return 28;
  if (n == "PxMat33")
    return 36;
  if (n == "PxMat44")
    return 64;
  return {};
}
bool builtin(const QualifiedName &t, const char *n) {
  return t.namespace_name == "physx3" && t.name == n;
}
std::uint64_t uint_at(const Bytes &b, std::size_t o, std::size_t w,
                      ByteOrder order) {
  if (w > 8 || o > b.size() || w > b.size() - o)
    throw std::out_of_range("property byte range");
  std::uint64_t v = 0;
  if (order == ByteOrder::little)
    for (std::size_t i = 0; i < w; ++i)
      v |= std::uint64_t(b[o + i]) << (8 * i);
  else
    for (std::size_t i = 0; i < w; ++i)
      v = (v << 8) | b[o + i];
  return v;
}
float float_at(const Bytes &b, std::size_t o, ByteOrder order) {
  auto bits = static_cast<std::uint32_t>(uint_at(b, o, 4, order));
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}
std::size_t item_offset(const PropertyValue &v, std::size_t i, std::size_t s) {
  if (i >= v.item_count || i > std::numeric_limits<std::size_t>::max() / s)
    throw std::out_of_range("property item index");
  return i * s;
}
PropertyValue make_value(QualifiedName type, std::uint64_t count, Bytes bytes,
                         const StreamHeader &h) {
  PropertyValue v;
  v.incoming_type = std::move(type);
  v.item_count = count;
  v.byte_order = h.byte_order;
  v.pointer_width = h.pointer_width();
  v.bytes = std::make_shared<const Bytes>(std::move(bytes));
  v.opaque = !builtin_size(v.incoming_type, v.pointer_width) ||
             builtin(v.incoming_type, "ObjectRef");
  return v;
}
} // namespace
const ClassDefinition *
Metadata::find_class(const QualifiedName &n) const noexcept {
  auto i = classes.find(n);
  return i == classes.end() ? nullptr : &i->second;
}
const PropertyDefinition *
Metadata::find_property(const QualifiedName &n,
                        const std::string &p) const noexcept {
  auto t = find_class(n);
  for (std::size_t d = 0; t && d <= classes.size(); ++d) {
    auto i = t->properties.find(p);
    if (i != t->properties.end())
      return &i->second;
    t = t->parent ? find_class(*t->parent) : nullptr;
  }
  return nullptr;
}
bool Metadata::is_a(const QualifiedName &d,
                    const QualifiedName &b) const noexcept {
  auto t = find_class(d);
  for (std::size_t depth = 0; t && depth <= classes.size(); ++depth) {
    if (t->name == b)
      return true;
    t = t->parent ? find_class(*t->parent) : nullptr;
  }
  return false;
}
const Bytes &PropertyValue::raw_bytes() const noexcept {
  static const Bytes empty;
  return bytes ? *bytes : empty;
}
std::optional<std::uint64_t> PropertyValue::as_uint(std::size_t i) const {
  const auto &n = incoming_type.name;
  if (incoming_type.namespace_name != "physx3" ||
      (n != "PvdU8" && n != "PvdU16" && n != "PvdU32" && n != "PvdU64" &&
       n != "PvdBool" && n != "StringHandle" && n != "VoidPtr"))
    return {};
  auto s = *builtin_size(incoming_type, pointer_width);
  return uint_at(raw_bytes(), item_offset(*this, i, s), s, byte_order);
}
std::optional<std::int64_t> PropertyValue::as_int(std::size_t i) const {
  const auto &n = incoming_type.name;
  if (incoming_type.namespace_name != "physx3" ||
      (n != "PvdI8" && n != "PvdI16" && n != "PvdI32" && n != "PvdI64"))
    return {};
  auto s = *builtin_size(incoming_type, pointer_width);
  auto bits = uint_at(raw_bytes(), item_offset(*this, i, s), s, byte_order);
  if (s < 8 && (bits & (std::uint64_t(1) << (s * 8 - 1))))
    bits |= (~std::uint64_t(0)) << (s * 8);
  std::int64_t result;
  std::memcpy(&result, &bits, 8);
  return result;
}
std::optional<float> PropertyValue::as_float(std::size_t i) const {
  if (!builtin(incoming_type, "PvdF32"))
    return {};
  return float_at(raw_bytes(), item_offset(*this, i, 4), byte_order);
}
std::optional<double> PropertyValue::as_double(std::size_t i) const {
  if (builtin(incoming_type, "PvdF32"))
    return static_cast<double>(*as_float(i));
  if (!builtin(incoming_type, "PvdF64"))
    return {};
  auto bits = uint_at(raw_bytes(), item_offset(*this, i, 8), 8, byte_order);
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}
std::optional<Vec3> PropertyValue::as_vec3(std::size_t i) const {
  if (!builtin(incoming_type, "PxVec3"))
    return {};
  auto o = item_offset(*this, i, 12);
  const auto &b = raw_bytes();
  return Vec3{float_at(b, o, byte_order), float_at(b, o + 4, byte_order),
              float_at(b, o + 8, byte_order)};
}
std::optional<Quat> PropertyValue::as_quat(std::size_t i) const {
  if (!builtin(incoming_type, "PxQuat"))
    return {};
  auto o = item_offset(*this, i, 16);
  const auto &b = raw_bytes();
  return Quat{float_at(b, o, byte_order), float_at(b, o + 4, byte_order),
              float_at(b, o + 8, byte_order), float_at(b, o + 12, byte_order)};
}
std::optional<Transform> PropertyValue::as_transform(std::size_t i) const {
  if (!builtin(incoming_type, "PxTransform"))
    return {};
  auto o = item_offset(*this, i, 28);
  const auto &b = raw_bytes();
  return Transform{
      {float_at(b, o, byte_order), float_at(b, o + 4, byte_order),
       float_at(b, o + 8, byte_order), float_at(b, o + 12, byte_order)},
      {float_at(b, o + 16, byte_order), float_at(b, o + 20, byte_order),
       float_at(b, o + 24, byte_order)}};
}
std::optional<std::uint64_t> PropertyValue::as_object_ref(std::size_t i) const {
  if (!reference_ids)
    return {};
  if (i >= reference_ids->size())
    throw std::out_of_range("object ref item index");
  return (*reference_ids)[i];
}
const PropertyValue *
InstanceState::property(const std::string &n) const noexcept {
  auto i = properties.find(n);
  return i == properties.end() ? nullptr : &i->second;
}
const InstanceState *Snapshot::instance(std::uint64_t id) const noexcept {
  auto i = instances.find(id);
  return i == instances.end() ? nullptr : &i->second;
}
SessionError::SessionError(std::string m, std::size_t i, std::uint64_t o)
    : std::runtime_error(std::move(m) + " at event " + std::to_string(i) +
                         " (byte " + std::to_string(o) + ")"),
      event_index_(i), byte_offset_(o) {}

struct Session::Impl {
  enum class Kind {
    create,
    destroy,
    assign,
    pickable,
    top_level,
    color,
    camera,
    origin
  };
  struct Operation {
    std::size_t event_count = 0;
    Kind kind = Kind::assign;
    std::uint64_t id = 0;
    InstanceState instance;
    std::vector<std::pair<std::string, PropertyValue>> assignments;
    bool flag = false;
    Color color;
    std::string camera_name;
    CameraState camera;
    Vec3 shift;
  };
  struct Chunk {
    std::uint64_t id;
    std::string property;
    QualifiedName type;
    Bytes data;
    std::uint64_t count = 0;
  };
  struct Section {
    std::size_t event_count;
    std::uint64_t timestamp;
  };
  StreamHeader header;
  SessionOptions options;
  std::vector<Event> events;
  Metadata metadata;
  std::vector<FrameMarker> frames;
  std::vector<SectionMarker> completed_sections;
  std::vector<ObjectLifetime> lifetimes;
  std::vector<ChannelPacket> channels;
  std::map<std::uint64_t, ProfileZoneInfo> zones;
  std::vector<Operation> operations;
  std::map<std::size_t, Snapshot> checkpoints;
  std::map<std::uint64_t, std::map<std::uint32_t, std::string>> strings;
  std::map<std::uint64_t, Chunk> chunks;
  std::map<std::uint64_t, QualifiedName> message_groups;
  std::map<std::tuple<std::uint64_t, std::uint64_t, std::string>, Section>
      sections;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> frame_counts;
  std::map<std::uint64_t, std::size_t> live_lifetime;
  std::map<std::uint64_t, std::uint64_t> generations;
  Snapshot working;
  std::size_t index = 0;
  std::uint64_t current_byte_offset = 0;
  bool is_finished = false, is_failed = false;
  bool has_consistent_state = true;
  void require_state() const {
    if (!has_consistent_state)
      throw SessionRecoveryError{};
  }
  [[noreturn]] void fail(const std::string &m) const {
    throw SessionError(m, index, current_byte_offset);
  }
  std::string str(std::uint64_t s, std::uint32_t h) const {
    if (!h)
      return {};
    auto it = strings.find(s);
    if (it == strings.end() || !it->second.count(h))
      fail("undefined string handle " + std::to_string(h) + " on stream " +
           std::to_string(s));
    return it->second.at(h);
  }
  QualifiedName name(std::uint64_t s, WireName n) const {
    return {str(s, n.namespace_handle), str(s, n.name_handle)};
  }
  ClassDefinition &cls(const QualifiedName &n) {
    auto i = metadata.classes.find(n);
    if (i == metadata.classes.end())
      fail("undefined class " + n.str());
    return i->second;
  }
  InstanceState &object(std::uint64_t id) {
    auto i = working.instances.find(id);
    if (i == working.instances.end())
      fail("instance not alive: " + std::to_string(id));
    return i->second;
  }
  const PropertyDefinition &prop(std::uint64_t id, const std::string &n) {
    auto p = metadata.find_property(object(id).class_name, n);
    if (!p)
      fail("undefined property " + object(id).class_name.str() + "." + n);
    return *p;
  }
  const MessageDefinition &message(const QualifiedName &n) {
    auto i = metadata.messages.find(n);
    if (i == metadata.messages.end())
      fail("undefined property message " + n.str());
    return i->second;
  }
  static void apply(Snapshot &s, const Operation &o) {
    switch (o.kind) {
    case Kind::create:
      s.instances.emplace(o.id, o.instance);
      break;
    case Kind::destroy:
      s.instances.erase(o.id);
      s.origin_shifts.erase(o.id);
      break;
    case Kind::assign:
      for (const auto &p : o.assignments)
        s.instances.at(o.id).properties[p.first] = p.second;
      break;
    case Kind::pickable:
      s.instances.at(o.id).pickable = o.flag;
      break;
    case Kind::top_level:
      s.instances.at(o.id).top_level = o.flag;
      break;
    case Kind::color:
      s.instances.at(o.id).color = o.color;
      break;
    case Kind::camera:
      s.cameras[o.camera_name] = o.camera;
      break;
    case Kind::origin:
      s.origin_shifts[o.id] = o.shift;
      break;
    }
  }
  void commit(Operation o) {
    o.event_count = index + 1;
    apply(working, o);
    operations.push_back(std::move(o));
  }
  std::pair<std::string, std::size_t> cstring(const Bytes &b, std::size_t o) {
    if (o >= b.size())
      fail("missing NUL-terminated string fixup");
    auto end = std::find(b.begin() + o, b.end(), std::uint8_t(0));
    if (end == b.end())
      fail("unterminated string fixup");
    return {std::string(b.begin() + o, end),
            static_cast<std::size_t>(end - b.begin()) + 1};
  }

  void validate_value(std::uint64_t stream, PropertyValue &v,
                      const PropertyDefinition &p) {
    cls(v.incoming_type);
    const auto &b = v.raw_bytes();
    if (p.property_type == 1 && v.item_count != 1)
      fail("scalar property requires exactly one item: " + p.name);
    if (builtin(v.incoming_type, "String")) {
      if (v.item_count != 1 || b.size() < header.pointer_width())
        fail("invalid pointer-string payload");
      auto decoded = cstring(b, header.pointer_width());
      if (decoded.second != b.size())
        fail("trailing pointer-string bytes");
      v.text = std::move(decoded.first);
    } else if (auto s = builtin_size(v.incoming_type, header.pointer_width())) {
      if (v.item_count > std::numeric_limits<std::size_t>::max() / *s ||
          b.size() != v.item_count * *s)
        fail("property size/type/item count mismatch: " + p.name);
      if (builtin(v.incoming_type, "StringHandle")) {
        if (v.item_count != 1)
          fail("SDK does not support StringHandle arrays");
        v.text = str(stream, static_cast<std::uint32_t>(
                                 uint_at(b, 0, 4, header.byte_order)));
      }
    }
    if (builtin(p.datatype, "ObjectRef") &&
        builtin(v.incoming_type, "VoidPtr")) {
      v.reference_ids = std::vector<std::uint64_t>();
      for (std::size_t i = 0; i < v.item_count; ++i)
        v.reference_ids->push_back(uint_at(b, i * header.pointer_width(),
                                           header.pointer_width(),
                                           header.byte_order));
    }
  }
  void channel(std::uint64_t stream, std::uint64_t id, const std::string &p,
               const PropertyValue &v) {
    const auto &d = prop(id, p);
    if (d.semantic == "profile event stream" ||
        d.semantic == "memory event stream" ||
        d.semantic == "render event stream")
      channels.push_back({index, stream, id, p, d.semantic, v});
  }
  void assign(std::uint64_t stream, std::uint64_t id, const std::string &p,
              QualifiedName t, std::uint64_t count, Bytes data) {
    auto value = make_value(std::move(t), count, std::move(data), header);
    validate_value(stream, value, prop(id, p));
    channel(stream, id, p, value);
    Operation op;
    op.kind = Kind::assign;
    op.id = id;
    op.assignments.emplace_back(p, std::move(value));
    commit(std::move(op));
  }
  void assign_message(std::uint64_t stream, std::uint64_t id,
                      const QualifiedName &type, const Bytes &data) {
    const auto &d = message(type);
    if (!metadata.is_a(object(id).class_name, d.class_name))
      fail("message class incompatible with instance: " + type.str());
    if (data.size() < d.byte_size)
      fail("message shorter than declared size: " + type.str());
    Operation op;
    op.kind = Kind::assign;
    op.id = id;
    std::size_t string_offset = d.byte_size;
    for (const auto &a : d.arguments) {
      if (a.offset > d.byte_size || a.byte_size > d.byte_size - a.offset)
        fail("message argument outside fixed payload");
      const auto &p = prop(id, a.property_name);
      auto size = builtin_size(a.datatype, header.pointer_width());
      std::size_t width = size.value_or(a.byte_size);
      if (width > a.byte_size)
        fail("message argument smaller than source type");
      Bytes bytes(data.begin() + a.offset, data.begin() + a.offset + width);
      auto v = make_value(a.datatype, 1, std::move(bytes), header);
      if (builtin(a.datatype, "String")) {
        auto decoded = cstring(data, string_offset);
        string_offset = decoded.second;
        v.text = std::move(decoded.first);
        Bytes raw = v.raw_bytes();
        raw.insert(raw.end(), v.text->begin(), v.text->end());
        raw.push_back(0);
        v.bytes = std::make_shared<const Bytes>(std::move(raw));
      }
      validate_value(stream, v, p);
      op.assignments.emplace_back(a.property_name, std::move(v));
    }
    if (string_offset != data.size())
      fail("unaccounted bytes after property message/string fixups");
    // Every field and all string fixups are validated before publishing any of
    // this message. A rejected later field cannot leak an earlier
    // channel/value.
    for (const auto &assignment : op.assignments)
      channel(stream, id, assignment.first, assignment.second);
    commit(std::move(op));
  }
  void refs(std::uint64_t id, const std::string &p, std::uint64_t ref,
            bool remove) {
    const auto &d = prop(id, p);
    if (d.property_type != 2 || !builtin(d.datatype, "ObjectRef"))
      fail("reference mutation requires ObjectRef array: " + p);
    std::vector<std::uint64_t> values;
    if (const auto *old = object(id).property(p)) {
      if (!old->reference_ids)
        fail("cannot mutate unresolved internal ObjectRef encoding");
      values = *old->reference_ids;
    }
    auto found = std::find(values.begin(), values.end(), ref);
    if (remove) {
      if (found == values.end())
        fail("removing absent reference");
      values.erase(found);
    } else
      values.push_back(ref);
    Bytes raw;
    for (auto v : values)
      for (std::size_t i = 0; i < header.pointer_width(); ++i) {
        auto shift = header.byte_order == ByteOrder::little
                         ? i
                         : header.pointer_width() - i - 1;
        raw.push_back(static_cast<std::uint8_t>(v >> (shift * 8)));
      }
    auto value = make_value({"physx3", "VoidPtr"}, values.size(),
                            std::move(raw), header);
    value.reference_ids = std::move(values);
    Operation op;
    op.kind = Kind::assign;
    op.id = id;
    op.assignments.emplace_back(p, std::move(value));
    commit(std::move(op));
  }
  void process(const Event &e) {
    auto stream = e.stream_id;
    std::visit(
        [&](const auto &v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, StringHandleEvent>) {
            if (!v.handle && !v.string.empty())
              fail("nonempty string uses reserved handle zero");
            auto [it, inserted] = strings[stream].emplace(v.handle, v.string);
            if (!inserted && it->second != v.string)
              fail("conflicting string handle definition");
          } else if constexpr (std::is_same_v<T, CreateClass>) {
            auto t = name(stream, v.name);
            metadata.classes.emplace(t, ClassDefinition{t, {}, {}});
          } else if constexpr (std::is_same_v<T, DeriveClass>) {
            auto p = name(stream, v.parent), c = name(stream, v.child);
            cls(p);
            auto &child = cls(c);
            if (p == c || metadata.is_a(p, c))
              fail("inheritance cycle");
            if (child.parent && *child.parent != p)
              fail("conflicting class parent");
            child.parent = p;
          } else if constexpr (std::is_same_v<T, CreateProperty>) {
            if (v.property_type != 1 && v.property_type != 2)
              fail("unsupported property kind");
            auto t = name(stream, v.class_name),
                 dt = name(stream, v.datatype_name);
            cls(dt);
            PropertyDefinition p{str(stream, v.name),
                                 str(stream, v.semantic),
                                 dt,
                                 v.property_type,
                                 {}};
            for (const auto &n : v.values)
              p.named_values.emplace(str(stream, n.name), n.value);
            auto &props = cls(t).properties;
            auto old = props.find(p.name);
            if (old != props.end() &&
                (old->second.datatype != p.datatype ||
                 old->second.property_type != p.property_type ||
                 old->second.semantic != p.semantic ||
                 old->second.named_values != p.named_values))
              fail("conflicting property definition");
            props[p.name] = std::move(p);
          } else if constexpr (std::is_same_v<T, CreatePropertyMessage>) {
            MessageDefinition d;
            d.name = name(stream, v.message_name);
            d.class_name = name(stream, v.class_name);
            d.byte_size = v.message_byte_size;
            cls(d.class_name);
            for (const auto &a : v.message_entries) {
              auto p = str(stream, a.property_name);
              auto t = name(stream, a.datatype_name);
              cls(t);
              if (!metadata.find_property(d.class_name, p))
                fail("message references undefined property " + p);
              if (a.message_offset > d.byte_size ||
                  a.byte_size > d.byte_size - a.message_offset)
                fail("message argument exceeds declared size");
              d.arguments.push_back({p, t, a.message_offset, a.byte_size});
            }
            if (!metadata.messages.emplace(d.name, std::move(d)).second)
              fail("duplicate property message definition");
          } else if constexpr (std::is_same_v<T, CreateInstance>) {
            auto t = name(stream, v.class_name);
            cls(t);
            if (working.instance(v.instance_id))
              fail("duplicate live instance id");
            auto gen = generations[v.instance_id]++;
            lifetimes.push_back({v.instance_id, gen, stream, t, index, {}});
            live_lifetime[v.instance_id] = lifetimes.size() - 1;
            Operation op;
            op.kind = Kind::create;
            op.id = v.instance_id;
            op.instance.id = v.instance_id;
            op.instance.generation = gen;
            op.instance.creating_stream = stream;
            op.instance.class_name = std::move(t);
            commit(std::move(op));
          } else if constexpr (std::is_same_v<T, DestroyInstance>) {
            object(v.instance_id);
            lifetimes.at(live_lifetime.at(v.instance_id)).destroyed_event =
                index;
            live_lifetime.erase(v.instance_id);
            Operation op;
            op.kind = Kind::destroy;
            op.id = v.instance_id;
            commit(std::move(op));
          } else if constexpr (std::is_same_v<T, SetPropertyValue>) {
            if (chunks.count(stream) || message_groups.count(stream))
              fail("property set inside active transaction");
            assign(stream, v.instance_id, str(stream, v.property_name),
                   name(stream, v.incoming_type_name), v.num_items, v.data);
          } else if constexpr (std::is_same_v<T, BeginSetPropertyValue>) {
            if (chunks.count(stream) || message_groups.count(stream))
              fail("nested stream transaction");
            auto p = str(stream, v.property_name);
            prop(v.instance_id, p);
            auto t = name(stream, v.incoming_type_name);
            cls(t);
            chunks.emplace(stream, Chunk{v.instance_id, p, t, {}, 0});
          } else if constexpr (std::is_same_v<T, AppendPropertyValueData>) {
            auto it = chunks.find(stream);
            if (it == chunks.end())
              fail("append without begin property transaction");
            auto &c = it->second;
            if (c.count >
                    std::numeric_limits<std::uint64_t>::max() - v.num_items ||
                v.data.size() > c.data.max_size() - c.data.size())
              fail("chunk size overflow");
            c.count += v.num_items;
            c.data.insert(c.data.end(), v.data.begin(), v.data.end());
          } else if constexpr (std::is_same_v<T, EndSetPropertyValue>) {
            auto it = chunks.find(stream);
            if (it == chunks.end())
              fail("end without begin property transaction");
            const auto &c = it->second;
            assign(stream, c.id, c.property, c.type, c.count, c.data);
            chunks.erase(it);
          } else if constexpr (std::is_same_v<T, SetPropertyMessage>) {
            if (chunks.count(stream) || message_groups.count(stream))
              fail("message set inside active transaction");
            assign_message(stream, v.instance_id, name(stream, v.message_name),
                           v.data);
          } else if constexpr (std::is_same_v<T, BeginPropertyMessageGroup>) {
            if (chunks.count(stream) || message_groups.count(stream))
              fail("nested stream transaction");
            auto t = name(stream, v.msg_name);
            for (const auto &a : message(t).arguments)
              if (builtin(a.datatype, "String"))
                fail("SDK message groups cannot contain strings");
            message_groups.emplace(stream, std::move(t));
          } else if constexpr (std::is_same_v<T,
                                              SendPropertyMessageFromGroup>) {
            auto it = message_groups.find(stream);
            if (it == message_groups.end())
              fail("message without begin group");
            assign_message(stream, v.instance, it->second, v.data);
          } else if constexpr (std::is_same_v<T, EndPropertyMessageGroup>) {
            if (!message_groups.erase(stream))
              fail("end without begin message group");
          } else if constexpr (std::is_same_v<T, PushBackObjectRef>) {
            refs(v.instance_id, str(stream, v.property), v.object_ref, false);
          } else if constexpr (std::is_same_v<T, RemoveObjectRef>) {
            refs(v.instance_id, str(stream, v.property), v.object_ref, true);
          } else if constexpr (std::is_same_v<T, BeginSection>) {
            auto n = str(stream, v.name);
            auto key = std::make_tuple(stream, v.section_id, n);
            if (!sections.emplace(key, Section{index + 1, v.timestamp}).second)
              fail("duplicate open section");
          } else if constexpr (std::is_same_v<T, EndSection>) {
            auto n = str(stream, v.name);
            auto key = std::make_tuple(stream, v.section_id, n);
            auto it = sections.find(key);
            if (it == sections.end())
              fail("section end has no matching begin on this stream");
            completed_sections.push_back({stream, v.section_id, n,
                                          it->second.event_count, index + 1,
                                          it->second.timestamp, v.timestamp});
            if (n == "frame") {
              auto ordinal = frame_counts[{stream, v.section_id}]++;
              frames.push_back({stream, v.section_id, ordinal,
                                it->second.event_count, index + 1,
                                it->second.timestamp, v.timestamp});
            }
            sections.erase(it);
          } else if constexpr (std::is_same_v<T, SetPickable>) {
            object(v.instance_id);
            Operation op;
            op.kind = Kind::pickable;
            op.id = v.instance_id;
            op.flag = v.pickable;
            commit(std::move(op));
          } else if constexpr (std::is_same_v<T, SetIsTopLevel>) {
            object(v.instance_id);
            Operation op;
            op.kind = Kind::top_level;
            op.id = v.instance_id;
            op.flag = v.is_top_level;
            commit(std::move(op));
          } else if constexpr (std::is_same_v<T, SetColor>) {
            object(v.instance_id);
            Operation op;
            op.kind = Kind::color;
            op.id = v.instance_id;
            op.color = v.color;
            commit(std::move(op));
          } else if constexpr (std::is_same_v<T, SetCamera>) {
            Operation op;
            op.kind = Kind::camera;
            op.camera_name = v.name;
            op.camera = {v.position, v.up, v.target};
            commit(std::move(op));
          } else if constexpr (std::is_same_v<T, OriginShift>) {
            object(v.instance_id);
            auto old = working.origin_shifts.find(v.instance_id);
            auto prev =
                old == working.origin_shifts.end() ? Vec3{} : old->second;
            Operation op;
            op.kind = Kind::origin;
            op.id = v.instance_id;
            op.shift = {prev.x + v.shift.x, prev.y + v.shift.y,
                        prev.z + v.shift.z};
            commit(std::move(op));
          } else if constexpr (std::is_same_v<T, AddProfileZone>) {
            auto &z = zones[v.instance_id];
            z.id = v.instance_id;
            z.name = v.name;
          } else if constexpr (std::is_same_v<T, AddProfileZoneEvent>) {
            auto &z = zones[v.instance_id];
            z.id = v.instance_id;
            z.events[v.event_id] = v;
          } else if constexpr (std::is_same_v<T, StreamEndEvent>) {
            if (chunks.count(stream) || message_groups.count(stream))
              fail("stream ended during active transaction");
          } else if constexpr (std::is_same_v<
                                   T, ErrorMessage>) { /* Preserved, with time
                                                          and source location,
                                                          in events(). */
          } else {
            static_assert(!std::is_same_v<T, T>, "unhandled wire event");
          }
        },
        e.payload);
  }

  Impl(StreamHeader h, SessionOptions o) : header(h), options(o) {
    if (header.stream_version != supported_stream_version ||
        (header.stream_flags & ~1u))
      throw std::invalid_argument("unsupported stream header for replay");
    for (const char *n :
         {"PvdI8",   "PvdU8",       "PvdI16",  "PvdU16",       "PvdI32",
          "PvdU32",  "PvdI64",      "PvdU64",  "PvdF32",       "PvdF64",
          "PvdBool", "PvdColor",    "String",  "StringHandle", "ObjectRef",
          "VoidPtr", "PxVec2",      "PxVec3",  "PxVec4",       "PxBounds3",
          "PxQuat",  "PxTransform", "PxMat33", "PxMat44",      "U32Array4"}) {
      QualifiedName t{"physx3", n};
      metadata.classes.emplace(t, ClassDefinition{t, {}, {}});
    }
    checkpoints.emplace(0, Snapshot{});
  }
  void append(std::vector<Event> batch) {
    if (is_finished)
      throw std::logic_error("cannot append to a finished session");
    if (is_failed)
      throw std::logic_error("cannot append to a failed session");
    try {
      index = events.size();
      current_byte_offset = batch.empty() ? 0 : batch.front().byte_offset;
      if (batch.size() > events.max_size() - events.size() ||
          events.size() > options.decode_limits.max_total_events ||
          batch.size() > options.decode_limits.max_total_events - events.size())
        fail("session event limit exceeded");
      const auto required = events.size() + batch.size();
      if (required > events.capacity()) {
        // Geometric growth is essential when the transport appends one event
        // at a time; reserve(size+1) would repeatedly move the whole history.
        auto capacity = events.capacity();
        auto grown = capacity <= events.max_size() / 2 ? capacity * 2
                                                       : events.max_size();
        events.reserve(std::max(required, grown));
      }
      for (auto &event : batch) {
        index = events.size();
        current_byte_offset = event.byte_offset;
        const auto old_channels = channels.size();
        const auto old_operations = operations.size();
        try {
          process(event);
        } catch (...) {
          // No semantic validation follows a state commit. Payload/channel
          // preparation may precede commit; remove any unpublished records.
          channels.resize(old_channels);
          operations.resize(old_operations);
          throw;
        }
        // Event's move is noexcept and capacity was reserved before mutation.
        static_assert(std::is_nothrow_move_constructible_v<Event>);
        events.push_back(std::move(event));
        working.event_count = events.size();
        if (options.checkpoint_interval && options.max_checkpoints &&
            events.size() % options.checkpoint_interval == 0 &&
            at_transaction_boundary()) {
          checkpoints.emplace(events.size(), working);
          while (checkpoints.size() - 1 > options.max_checkpoints)
            checkpoints.erase(std::next(checkpoints.begin()));
        }
      }
    } catch (...) {
      is_failed = true;
      throw;
    }
  }
  bool at_transaction_boundary() const noexcept {
    return chunks.empty() && message_groups.empty();
  }
  void finish() {
    if (is_failed)
      throw std::logic_error("cannot finish a failed session");
    if (is_finished)
      return;
    if (!at_transaction_boundary()) {
      is_failed = true;
      index = events.size();
      current_byte_offset = events.empty() ? 0 : events.back().byte_offset;
      fail("capture ended during active property transaction");
    }
    // An ordinary section may be open when a recording is stopped. Completed
    // section/frame markers remain queryable; property transactions must close.
    is_finished = true;
  }
};
Session::Session(StreamHeader h, SessionOptions o)
    : impl_(std::make_unique<Impl>(h, o)) {}
Session::Session(StreamHeader h, std::vector<Event> e, SessionOptions o)
    : Session(h, o) {
  append(std::move(e));
  finish();
}
Session::~Session() = default;
Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;
Session Session::parse(const Bytes &b, SessionOptions o) {
  StreamDecoder d(o.decode_limits);
  d.feed(b);
  d.finish();
  if (!d.header())
    throw std::invalid_argument("capture has no stream header");
  return Session(*d.header(), d.events(), o);
}
Session Session::load_file(const std::string &path, SessionOptions o) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot open capture: " + path);
  StreamDecoder d(o.decode_limits);
  std::array<std::uint8_t, 65536> b{};
  while (f) {
    f.read(reinterpret_cast<char *>(b.data()), b.size());
    auto n = f.gcount();
    if (n > 0)
      d.feed(b.data(), static_cast<std::size_t>(n));
  }
  if (!f.eof())
    throw std::runtime_error("cannot read capture: " + path);
  d.finish();
  if (!d.header())
    throw std::invalid_argument("capture has no stream header");
  return Session(*d.header(), d.events(), o);
}
void Session::append(std::vector<Event> events) {
  if (impl_->is_finished)
    throw std::logic_error("cannot append to a finished session");
  if (impl_->is_failed)
    throw std::logic_error("cannot append to a failed session");
  try {
    impl_->append(std::move(events));
  } catch (...) {
    // Normal append never replays old events. On an exception only, reconstruct
    // every public index and the working state from the already accepted
    // source. Do not finish: the valid prefix may include an open chunk/message
    // group.
    auto original_error = std::current_exception();
    try {
      auto recovered = std::make_unique<Impl>(impl_->header, impl_->options);
      recovered->append(impl_->events);
      recovered->is_failed = true;
      impl_.swap(recovered);
    } catch (...) {
      // Persistent resource exhaustion can prevent even reconstruction. Keep
      // the accepted source and status accessible, but never expose stale or
      // partially committed state/index records through a query.
      impl_->has_consistent_state = false;
      impl_->is_failed = true;
    }
    std::rethrow_exception(original_error);
  }
}
void Session::finish() { impl_->finish(); }
std::size_t Session::event_count() const noexcept {
  return impl_->events.size();
}
bool Session::finished() const noexcept { return impl_->is_finished; }
bool Session::failed() const noexcept { return impl_->is_failed; }
bool Session::state_available() const noexcept {
  return impl_->has_consistent_state;
}
bool Session::at_transaction_boundary() const noexcept {
  return impl_->has_consistent_state && impl_->at_transaction_boundary();
}
const StreamHeader &Session::header() const noexcept { return impl_->header; }
const Metadata &Session::metadata() const {
  impl_->require_state();
  return impl_->metadata;
}
const std::vector<Event> &Session::events() const noexcept {
  return impl_->events;
}
const std::vector<FrameMarker> &Session::frames() const {
  impl_->require_state();
  return impl_->frames;
}
const std::vector<SectionMarker> &Session::sections() const {
  impl_->require_state();
  return impl_->completed_sections;
}
const std::vector<ObjectLifetime> &Session::lifetimes() const {
  impl_->require_state();
  return impl_->lifetimes;
}
const std::vector<ChannelPacket> &Session::channels() const {
  impl_->require_state();
  return impl_->channels;
}
const std::map<std::uint64_t, ProfileZoneInfo> &Session::profile_zones() const {
  impl_->require_state();
  return impl_->zones;
}
Snapshot Session::snapshot_at_event(std::size_t count) const {
  impl_->require_state();
  if (count > impl_->events.size())
    throw std::out_of_range("snapshot event count exceeds capture");
  auto cp = impl_->checkpoints.upper_bound(count);
  --cp;
  Snapshot state = cp->second;
  auto first =
      std::upper_bound(impl_->operations.begin(), impl_->operations.end(),
                       cp->first, [](std::size_t i, const Impl::Operation &op) {
                         return i < op.event_count;
                       });
  for (auto it = first;
       it != impl_->operations.end() && it->event_count <= count; ++it)
    Impl::apply(state, *it);
  state.event_count = count;
  return state;
}
Snapshot Session::snapshot_at_frame(std::size_t frame) const {
  impl_->require_state();
  if (frame >= impl_->frames.size())
    throw std::out_of_range("frame index exceeds capture");
  return snapshot_at_event(impl_->frames[frame].event_count);
}
Snapshot Session::snapshot_at_frame(std::uint64_t stream, std::uint64_t scene,
                                    std::size_t frame) const {
  impl_->require_state();
  for (const auto &f : impl_->frames)
    if (f.stream_id == stream && f.scene_id == scene && f.index == frame)
      return snapshot_at_event(f.event_count);
  throw std::out_of_range("frame not found for stream/scene");
}
Snapshot Session::latest() const {
  return snapshot_at_event(impl_->events.size());
}
std::size_t Session::checkpoint_count() const {
  impl_->require_state();
  return impl_->checkpoints.size() - 1;
}
} // namespace pvd
