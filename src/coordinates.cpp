#include "coordinates_internal.hpp"
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace pvd::coordinates {
namespace {
bool is(const Metadata &m, const InstanceState &o, const char *name) {
  (void)m;
  if (o.class_name.namespace_name != "physx3")
    return false;
  const auto &n = o.class_name.name;
  const std::string_view root(name);
  if (n == root)
    return true;
  if (root == "PxRigidActor")
    return n == "PxRigidStatic" || n == "PxRigidDynamic" ||
           n == "PxArticulationLink";
  if (root == "PxParticleBase")
    return n == "PxParticleSystem" || n == "PxParticleFluid";
  if (root == "PxJoint")
    return n == "PxFixedJoint" || n == "PxDistanceJoint" ||
           n == "PxPrismaticJoint" || n == "PxRevoluteJoint" ||
           n == "PxSphericalJoint" || n == "PxD6Joint";
  return false;
}
std::optional<std::uint64_t> ref(const InstanceState &o, const char *name) {
  const auto *p = o.property(name);
  if (!p || p->item_count != 1 || !p->reference_ids ||
      p->reference_ids->size() != 1)
    return {};
  return p->reference_ids->front();
}
std::optional<std::uint64_t> scene(const Metadata &m, const Snapshot &s,
                                   const InstanceState &o,
                                   std::size_t depth = 0) {
  if (depth > 256 || depth > s.instances.size())
    return {};
  if (is(m, o, "PxScene"))
    return o.id;
  if (is(m, o, "PxRigidActor") || is(m, o, "PxArticulation") ||
      is(m, o, "PxCloth") || is(m, o, "PxParticleBase")) {
    if (auto r = ref(o, "Scene"); r && *r) {
      const auto *p = s.instance(*r);
      if (p && is(m, *p, "PxScene"))
        return *r;
    }
    if (is(m, o, "PxArticulationLink"))
      if (auto r = ref(o, "Parent"); r && *r)
        if (const auto *p = s.instance(*r))
          return scene(m, s, *p, depth + 1);
    return {};
  }
  if (is(m, o, "PxJoint")) {
    std::optional<std::uint64_t> result;
    for (const auto *name : {"Actors.actor0", "Actors.actor1"})
      if (auto r = ref(o, name); r && *r) {
        const auto *a = s.instance(*r);
        if (!a || !is(m, *a, "PxRigidActor"))
          return {};
        const auto v = scene(m, s, *a, depth + 1);
        if (!v || (result && result != v))
          return {};
        result = v;
      }
    return result;
  }
  return {};
}
float read(const Bytes &bytes, std::size_t at, ByteOrder order) {
  if (at > bytes.size() || 4 > bytes.size() - at)
    throw std::out_of_range("native coordinate byte range");
  std::uint32_t bits = 0;
  for (unsigned i = 0; i < 4; ++i)
    bits |= std::uint32_t(bytes[at + i])
            << (8 * (order == ByteOrder::little ? i : 3 - i));
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}
void write(Bytes &bytes, std::size_t at, float v, ByteOrder order) {
  if (!std::isfinite(v))
    throw std::invalid_argument(
        "OriginShift produced nonfinite native coordinate");
  std::uint32_t bits;
  std::memcpy(&bits, &v, 4);
  for (unsigned i = 0; i < 4; ++i)
    bytes.at(at + i) =
        std::uint8_t(bits >> (8 * (order == ByteOrder::little ? i : 3 - i)));
}
std::size_t offset(const PropertyValue &v, std::size_t item,
                   std::size_t stride) {
  if (item >= v.item_count ||
      item > std::numeric_limits<std::size_t>::max() / stride)
    throw std::out_of_range("native coordinate item index");
  return item * stride;
}
void shifted(PropertyValue &p, Vec3 shift, std::size_t stride,
             std::size_t position_offset) {
  const auto &source = native_bytes(p);
  if (p.item_count > std::numeric_limits<std::size_t>::max() / stride ||
      source.size() != p.item_count * stride)
    throw std::invalid_argument(
        "OriginShift coordinate property size mismatch");
  auto bytes = std::make_shared<Bytes>(source);
  for (std::size_t i = 0; i < p.item_count; ++i) {
    const auto at = i * stride + position_offset;
    // Deliberately round after every individual shift, just as
    // PxVec3::operator-=.
    write(*bytes, at, read(*bytes, at, p.byte_order) - shift.x, p.byte_order);
    write(*bytes, at + 4, read(*bytes, at + 4, p.byte_order) - shift.y,
          p.byte_order);
    write(*bytes, at + 8, read(*bytes, at + 8, p.byte_order) - shift.z,
          p.byte_order);
  }
  p.native_coordinate_bytes = std::move(bytes);
}
} // namespace
const Bytes &native_bytes(const PropertyValue &v) noexcept {
  return v.native_coordinate_bytes ? *v.native_coordinate_bytes : v.raw_bytes();
}
PropertyValue native_value(const PropertyValue &v) {
  auto result = v;
  if (result.native_coordinate_bytes)
    result.bytes = std::move(result.native_coordinate_bytes);
  return result;
}
std::optional<Transform> native_transform(const PropertyValue &v,
                                          std::size_t item) {
  if (v.incoming_type != QualifiedName{"physx3", "PxTransform"})
    return {};
  const auto at = offset(v, item, 28);
  const auto &b = native_bytes(v);
  return Transform{
      {read(b, at, v.byte_order), read(b, at + 4, v.byte_order),
       read(b, at + 8, v.byte_order), read(b, at + 12, v.byte_order)},
      {read(b, at + 16, v.byte_order), read(b, at + 20, v.byte_order),
       read(b, at + 24, v.byte_order)}};
}
std::optional<Vec3> native_vec3(const PropertyValue &v, std::size_t item) {
  if (v.incoming_type != QualifiedName{"physx3", "PxVec3"})
    return {};
  const auto at = offset(v, item, 12);
  const auto &b = native_bytes(v);
  return Vec3{read(b, at, v.byte_order), read(b, at + 4, v.byte_order),
              read(b, at + 8, v.byte_order)};
}
namespace detail {
bool query_publication(const Metadata &m, const InstanceState &o,
                       const std::string &p) {
  return is(m, o, "PxScene") &&
         (p == "SceneQueries.Raycasts" || p == "SceneQueries.Sweeps" ||
          p == "SceneQueries.Overlaps" || p == "BatchedQueries.Raycasts" ||
          p == "BatchedQueries.Sweeps" || p == "BatchedQueries.Overlaps");
}
bool coordinate_field(const Metadata &m, const InstanceState &o,
                      const std::string &p) {
  if (p == "GlobalPose")
    return is(m, o, "PxRigidActor") || is(m, o, "PxCloth");
  if (p == "Positions" || p == "ProjectionPlane")
    return is(m, o, "PxParticleBase");
  if (p == "LocalPose[eACTOR0]" || p == "LocalPose[eACTOR1]")
    return is(m, o, "PxJoint");
  return false;
}
bool epoch_field(const Metadata &m, const InstanceState &o,
                 const std::string &p) {
  return coordinate_field(m, o, p) || query_publication(m, o, p);
}
void apply_shift(const Metadata &m, Snapshot &s, std::uint64_t scene_id,
                 Vec3 shift) {
  if (!std::isfinite(shift.x) || !std::isfinite(shift.y) ||
      !std::isfinite(shift.z))
    throw std::invalid_argument("OriginShift vector must be finite");
  // Membership is evaluated at this event, including articulation Parent
  // chains. Later scene changes must not retroactively change this decision.
  for (auto &[id, o] : s.instances) {
    (void)id;
    if (scene(m, s, o) != std::optional<std::uint64_t>(scene_id))
      continue;
    if (o.coordinate_epoch == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("OriginShift coordinate epoch exhausted");
    ++o.coordinate_epoch;
    for (auto &[p, value] : o.properties) {
      if (!coordinate_field(m, o, p))
        continue;
      if (p == "LocalPose[eACTOR0]" &&
          ref(o, "Actors.actor0") != std::optional<std::uint64_t>(0))
        continue;
      if (p == "LocalPose[eACTOR1]" &&
          ref(o, "Actors.actor1") != std::optional<std::uint64_t>(0))
        continue;
      if (p == "ProjectionPlane") {
        if (value.incoming_type != QualifiedName{"physx3", "PxVec4"} ||
            value.item_count != 1 || native_bytes(value).size() != 16)
          throw std::invalid_argument(
              "OriginShift particle ProjectionPlane must be one PxVec4");
        auto bytes = std::make_shared<Bytes>(native_bytes(value));
        const float delta = (read(*bytes, 0, value.byte_order) * shift.x +
                             read(*bytes, 4, value.byte_order) * shift.y) +
                            read(*bytes, 8, value.byte_order) * shift.z;
        write(*bytes, 12, read(*bytes, 12, value.byte_order) + delta,
              value.byte_order);
        value.native_coordinate_bytes = std::move(bytes);
        continue;
      }
      const auto transform = p != "Positions";
      if (value.incoming_type !=
          QualifiedName{"physx3", transform ? "PxTransform" : "PxVec3"})
        throw std::invalid_argument(
            "OriginShift native property has invalid type: " + p);
      shifted(value, shift, transform ? 28 : 12, transform ? 16 : 0);
    }
  }
}
} // namespace detail
} // namespace pvd::coordinates
