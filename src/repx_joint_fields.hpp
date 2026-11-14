#pragma once
// Internal, SDK-free schema for PhysX 3.4's six extension joint serializers.
#include "pvd/repx.hpp"
#include <array>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pvd::repx::joint_detail {
constexpr float pi = 3.14159265358979323846f;
using Value = std::variant<float, Vec3, Transform, std::string>;
struct Field {
  std::vector<std::string> path;
  Value value;
};
struct Joint {
  const InstanceState *object = nullptr;
  std::string type;
  std::array<std::uint64_t, 2> actors{};
  std::vector<Field> fields;
};
inline std::string where(const InstanceState &o, const std::string &p) {
  return o.class_name.str() + " #" + std::to_string(o.id) + "." + p + ": ";
}
inline const PropertyValue &need(const InstanceState &o, const std::string &p) {
  const auto *v = o.property(p);
  if (!v || v->item_count != 1)
    throw ExportError(where(o, p) + "missing/non-scalar recorded property");
  return *v;
}
inline float scalar(const InstanceState &o, const std::string &p) {
  const auto v = need(o, p).as_float();
  if (!v || !std::isfinite(*v))
    throw ExportError(where(o, p) + "expected finite float");
  return *v;
}
inline std::uint64_t integer(const InstanceState &o, const std::string &p) {
  const auto v = need(o, p).as_uint();
  if (!v)
    throw ExportError(where(o, p) + "expected unsigned integer");
  return *v;
}
inline float nonnegative(const InstanceState &o, const std::string &p) {
  const auto v = scalar(o, p);
  if (v < 0)
    throw ExportError(where(o, p) +
                      "negative value is not valid in SDK joint setter");
  return v;
}
inline Transform pose(const InstanceState &o, const std::string &p) {
  const auto v = need(o, p).as_transform();
  if (!v)
    throw ExportError(where(o, p) + "expected transform");
  const auto &q = v->rotation;
  const auto n = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z +
                 double(q.w) * q.w;
  if (!std::isfinite(n) || std::abs(n - 1) > 1e-3 ||
      !std::isfinite(v->position.x) || !std::isfinite(v->position.y) ||
      !std::isfinite(v->position.z))
    throw ExportError(where(o, p) + "invalid joint transform");
  return *v;
}
inline Vec3 vector(const InstanceState &o, const std::string &p) {
  const auto v = need(o, p).as_vec3();
  if (!v || !std::isfinite(v->x) || !std::isfinite(v->y) ||
      !std::isfinite(v->z))
    throw ExportError(where(o, p) + "expected finite Vec3");
  return *v;
}
inline std::array<std::uint64_t, 2> endpoints(const InstanceState &o) {
  std::array<std::uint64_t, 2> a{};
  for (std::size_t i = 0; i < 2; ++i) {
    const auto p = "Actors.actor" + std::to_string(i);
    const auto v = need(o, p).as_object_ref();
    if (!v)
      throw ExportError(where(o, p) + "unresolved actor reference");
    a[i] = *v;
  }
  if (a[0] == a[1])
    throw ExportError(
        where(o, "Actors") +
        "SDK joint requires distinct actors and at least one non-world actor");
  return a;
}
inline bool supported(const InstanceState &o) {
  if (o.class_name.namespace_name != "physx3")
    return false;
  const auto &n = o.class_name.name;
  return n == "PxFixedJoint" || n == "PxDistanceJoint" ||
         n == "PxPrismaticJoint" || n == "PxRevoluteJoint" ||
         n == "PxSphericalJoint" || n == "PxD6Joint";
}
inline std::vector<std::string> path(const std::string &p) {
  std::vector<std::string> result;
  std::string word;
  for (char c : p) {
    if (c == '.' || c == '[' || c == ']') {
      if (!word.empty()) {
        result.push_back(std::move(word));
        word.clear();
      }
    } else
      word += c;
  }
  if (!word.empty())
    result.push_back(std::move(word));
  return result;
}
inline std::string
flags(std::uint64_t v,
      std::initializer_list<std::pair<std::uint64_t, const char *>> names) {
  std::string out;
  for (const auto &n : names)
    if (v & n.first) {
      if (!out.empty())
        out += '|';
      out += n.second;
      v &= ~n.first;
    }
  if (v)
    throw ExportError("unknown PhysX 3.4 joint flag bits");
  return out.empty() ? "0" : out;
}
inline Joint read(const InstanceState &o) {
  if (!supported(o))
    throw ExportError(where(o, "class") +
                      "unsupported custom/unknown joint class");
  Joint j{&o, o.class_name.name, endpoints(o), {}};
  auto put = [&](const std::string &p, Value v) {
    j.fields.push_back({path(p), std::move(v)});
  };
  auto nonneg = [&](const std::string &p) {
    const auto v = nonnegative(o, p);
    put(p, v);
    return v;
  };
  auto angular_projection = [&]() {
    const auto v = nonneg("ProjectionAngularTolerance");
    if (v > pi)
      throw ExportError(where(o, "ProjectionAngularTolerance") + "exceeds Pi");
  };
  auto base_limit = [&](const std::string &prefix) {
    const auto v = nonneg(prefix + ".Restitution");
    if (v > 1)
      throw ExportError(where(o, prefix) + "restitution exceeds one");
    for (const auto *n :
         {"BounceThreshold", "Stiffness", "Damping", "ContactDistance"})
      nonneg(prefix + "." + n);
  };
  auto pair_limit = [&](const std::string &p, bool angular, bool d6 = false) {
    base_limit(p);
    const auto lower = scalar(o, p + ".Lower"), upper = scalar(o, p + ".Upper"),
               contact = scalar(o, p + ".ContactDistance");
    const float span = upper - lower;
    if (upper < lower || !std::isfinite(span) || contact > span)
      throw ExportError(where(o, p) +
                        "invalid limit pair range/contact distance");
    if (angular && (lower <= -(d6 ? 2 * pi : pi) ||
                    upper >= (d6 ? 2 * pi : pi) || span >= 2 * pi))
      throw ExportError(where(o, p) + "angular bounds exceed SDK range");
    put(p + ".Lower", lower);
    put(p + ".Upper", upper);
  };
  auto cone = [&](const std::string &p) {
    base_limit(p);
    for (const auto *axis : {"YAngle", "ZAngle"}) {
      const auto v = scalar(o, p + "." + axis);
      if (v <= 0 || v >= pi)
        throw ExportError(where(o, p) +
                          "cone angles must be between zero and Pi");
      put(p + "." + axis, v);
    }
  };
  put("LocalPose[eACTOR0]", pose(o, "LocalPose[eACTOR0]"));
  put("LocalPose[eACTOR1]", pose(o, "LocalPose[eACTOR1]"));
  nonneg("BreakForce.force");
  nonneg("BreakForce.torque");
  const auto f = integer(o, "ConstraintFlags");
  if (f & 1)
    throw ExportError(where(o, "ConstraintFlags") +
                      "broken joint state is read-only and cannot be restored "
                      "through SDK RepX");
  if (f & 512)
    throw ExportError(
        where(o, "ConstraintFlags") +
        "internal GPU-compatible flag cannot be set through SDK RepX");
  put("ConstraintFlags", flags(f, {{2, "ePROJECT_TO_ACTOR0"},
                                   {4, "ePROJECT_TO_ACTOR1"},
                                   {8, "eCOLLISION_ENABLED"},
                                   {16, "eVISUALIZATION"},
                                   {32, "eDRIVE_LIMITS_ARE_FORCES"},
                                   {128, "eIMPROVED_SLERP"},
                                   {256, "eDISABLE_PREPROCESSING"}}));
  for (const auto *p : {"InvMassScale0", "InvInertiaScale0", "InvMassScale1",
                        "InvInertiaScale1"})
    nonneg(p);
  std::string name;
  if (const auto *value = o.property("Name")) {
    if (value->item_count != 1)
      throw ExportError(where(o, "Name") + "expected scalar name");
    if (!value->text)
      throw ExportError(where(o, "Name") + "unresolved string");
    name = *value->text;
  }
  put("Name", std::move(name));
  if (j.type == "PxDistanceJoint") {
    for (const auto *p :
         {"MinDistance", "MaxDistance", "Tolerance", "Stiffness", "Damping"})
      nonneg(p);
    put("DistanceJointFlags",
        flags(integer(o, "DistanceJointFlags"), {{2, "eMAX_DISTANCE_ENABLED"},
                                                 {4, "eMIN_DISTANCE_ENABLED"},
                                                 {8, "eSPRING_ENABLED"}}));
  } else if (j.type == "PxFixedJoint") {
    nonneg("ProjectionLinearTolerance");
    angular_projection();
  } else if (j.type == "PxPrismaticJoint") {
    pair_limit("Limit", false);
    put("PrismaticJointFlags",
        flags(integer(o, "PrismaticJointFlags"), {{2, "eLIMIT_ENABLED"}}));
    nonneg("ProjectionLinearTolerance");
    angular_projection();
  } else if (j.type == "PxRevoluteJoint") {
    pair_limit("Limit", true);
    put("DriveVelocity", scalar(o, "DriveVelocity"));
    nonneg("DriveForceLimit");
    if (nonneg("DriveGearRatio") <= 0)
      throw ExportError(where(o, "DriveGearRatio") + "must be positive");
    put("RevoluteJointFlags",
        flags(integer(o, "RevoluteJointFlags"), {{1, "eLIMIT_ENABLED"},
                                                 {2, "eDRIVE_ENABLED"},
                                                 {4, "eDRIVE_FREESPIN"}}));
    nonneg("ProjectionLinearTolerance");
    angular_projection();
  } else if (j.type == "PxSphericalJoint") {
    cone("LimitCone");
    put("SphericalJointFlags",
        flags(integer(o, "SphericalJointFlags"), {{2, "eLIMIT_ENABLED"}}));
    nonneg("ProjectionLinearTolerance");
  } else {
    const char *motions[] = {"eLOCKED", "eLIMITED", "eFREE"};
    for (const auto *axis :
         {"eX", "eY", "eZ", "eTWIST", "eSWING1", "eSWING2"}) {
      const auto p = std::string("Motion[") + axis + "]";
      const auto v = integer(o, p);
      if (v > 2)
        throw ExportError(where(o, p) + "unknown D6 motion enum");
      put(p, std::string(motions[v]));
    }
    base_limit("LinearLimit");
    if (nonneg("LinearLimit.Value") <= 0)
      throw ExportError(where(o, "LinearLimit.Value") + "must be positive");
    pair_limit("TwistLimit", true, true);
    cone("SwingLimit");
    for (const auto *axis : {"eX", "eY", "eZ", "eSWING", "eTWIST", "eSLERP"}) {
      const auto prefix = std::string("Drive[") + axis + "]";
      nonneg(prefix + ".Stiffness");
      nonneg(prefix + ".Damping");
      nonneg(prefix + ".ForceLimit");
      put(prefix + ".Flags",
          flags(integer(o, prefix + ".Flags"), {{1, "eACCELERATION"}}));
    }
    put("DrivePosition", pose(o, "DrivePosition"));
    put("DriveVelocity.linear", vector(o, "DriveVelocity.linear"));
    put("DriveVelocity.angular", vector(o, "DriveVelocity.angular"));
    nonneg("ProjectionLinearTolerance");
    angular_projection();
  }
  return j;
}
} // namespace pvd::repx::joint_detail
