#pragma once
// SDK-free schema and graph validation for PhysX 3.4 native articulations.
#include "repx_joint_fields.hpp"
#include <map>
#include <set>

namespace pvd::repx::articulation_detail {
using Value =
    std::variant<std::uint64_t, float, Vec3, Quat, Transform, std::string>;
struct Field {
  std::vector<std::string> path;
  Value value;
};
struct Link {
  const InstanceState *object = nullptr, *joint = nullptr;
  std::uint64_t parent = 0;
  std::vector<Field> joint_fields;
};
struct Articulation {
  const InstanceState *object = nullptr;
  std::vector<Link> links; // Topological parent-before-child order.
  std::vector<Field> fields;
};
struct Aggregate {
  const InstanceState *object = nullptr;
  std::vector<std::uint64_t> actors, articulations;
  std::uint64_t max_actors = 0, actor_count = 0;
  bool self_collision = false;
};
inline bool is(const Metadata &m, const InstanceState &o, const char *type) {
  return m.is_a(o.class_name, {"physx3", type});
}
inline const InstanceState &resolve(const Snapshot &s, std::uint64_t id) {
  const auto *o = s.instance(id);
  if (!o)
    throw ExportError("missing articulation/aggregate dependency #" +
                      std::to_string(id));
  return *o;
}
inline std::uint64_t reference(const InstanceState &o, const std::string &p) {
  const auto value = joint_detail::need(o, p).as_object_ref();
  if (!value)
    throw ExportError(joint_detail::where(o, p) +
                      "invalid object-reference encoding");
  return *value;
}
inline std::vector<std::uint64_t>
references(const InstanceState &o, const char *p, bool optional = false) {
  const auto *value = o.property(p);
  if (!value && optional)
    return {};
  if (!value || value->item_count > 128)
    throw ExportError(joint_detail::where(o, p) +
                      "missing/oversized reference array");
  std::vector<std::uint64_t> out;
  std::set<std::uint64_t> seen;
  for (std::size_t i = 0; i < value->item_count; ++i) {
    const auto id = value->as_object_ref(i);
    if (!id || !*id || !seen.insert(*id).second)
      throw ExportError(joint_detail::where(o, p) +
                        "null/duplicate/unresolved reference");
    out.push_back(*id);
  }
  return out;
}
inline std::uint64_t owner_of(const Metadata &m, const Snapshot &s,
                              const InstanceState &link) {
  const auto *current = &link;
  std::set<std::uint64_t> seen;
  for (std::size_t depth = 0; depth <= 64; ++depth) {
    if (!seen.insert(current->id).second)
      throw ExportError("cycle in articulation parent graph");
    if (is(m, *current, "PxArticulation"))
      return current->id;
    if (!is(m, *current, "PxArticulationLink"))
      throw ExportError("articulation parent has wrong class");
    current = &resolve(s, reference(*current, "Parent"));
  }
  throw ExportError("articulation parent graph exceeds SDK 64-link limit");
}
inline std::vector<Field> joint_fields(const InstanceState &o) {
  if (o.class_name != QualifiedName{"physx3", "PxArticulationJoint"})
    throw ExportError("unsupported articulation inbound joint class");
  std::vector<Field> fields;
  const auto put = [&](const std::string &p, Value v) {
    fields.push_back({joint_detail::path(p), std::move(v)});
  };
  put("ParentPose", joint_detail::pose(o, "ParentPose"));
  put("ChildPose", joint_detail::pose(o, "ChildPose"));
  const auto drive = joint_detail::integer(o, "DriveType");
  if (drive > 1)
    throw ExportError("unknown articulation joint drive type");
  const auto q = joint_detail::need(o, "TargetOrientation").as_quat();
  if (!q)
    throw ExportError("missing articulation target orientation");
  const auto n = double(q->x) * q->x + double(q->y) * q->y +
                 double(q->z) * q->z + double(q->w) * q->w;
  if (!std::isfinite(n) || std::abs(std::sqrt(n) - 1) >= 1e-4 ||
      (drive == 1 && q->w != 0))
    throw ExportError("SDK RepX reads TargetOrientation before DriveType; a "
                      "nonunit eERROR target cannot be restored");
  put("TargetOrientation", *q);
  put("TargetVelocity", joint_detail::vector(o, "TargetVelocity"));
  put("DriveType", std::string(drive ? "eERROR" : "eTARGET"));
  for (const auto *p :
       {"Stiffness", "Damping", "TangentialStiffness", "TangentialDamping",
        "SwingLimitContactDistance", "TwistLimitContactDistance"})
    put(p, joint_detail::nonnegative(o, p));
  for (const auto *p : {"InternalCompliance", "ExternalCompliance"}) {
    const auto v = joint_detail::nonnegative(o, p);
    if (v <= 0)
      throw ExportError("articulation compliance must be positive");
    put(p, v);
  }
  // These labels are the generated SDK schema: zLimit stores getSwingLimit's
  // first (Y) result, and yLimit its second (Z) result. Do not rename/swap
  // them.
  for (const auto *p : {"SwingLimit.zLimit", "SwingLimit.yLimit"}) {
    const auto v = joint_detail::scalar(o, p);
    if (v <= 0 || v >= joint_detail::pi)
      throw ExportError("invalid articulation swing limit");
    put(p, v);
  }
  const auto lower = joint_detail::scalar(o, "TwistLimit.lower"),
             upper = joint_detail::scalar(o, "TwistLimit.upper");
  if (lower >= upper || lower <= -joint_detail::pi || upper >= joint_detail::pi)
    throw ExportError("invalid articulation twist limit");
  put("TwistLimit.lower", lower);
  put("TwistLimit.upper", upper);
  for (const auto *p : {"SwingLimitEnabled", "TwistLimitEnabled"}) {
    const auto value = joint_detail::integer(o, p);
    if (value > 1)
      throw ExportError("invalid articulation joint boolean");
    put(p, std::string(value ? "true" : "false"));
  }
  return fields;
}
inline Articulation read(const Metadata &, const Snapshot &s,
                         const InstanceState &o,
                         const std::set<std::uint64_t> &known_links) {
  if (o.class_name != QualifiedName{"physx3", "PxArticulation"})
    throw ExportError("unsupported articulation class");
  Articulation result;
  result.object = &o;
  const auto put = [&](const std::string &p, Value v) {
    result.fields.push_back({joint_detail::path(p), std::move(v)});
  };
  for (const auto *p : {"MaxProjectionIterations", "InternalDriveIterations",
                        "ExternalDriveIterations"}) {
    const auto v = joint_detail::integer(o, p);
    if (v > UINT32_MAX)
      throw ExportError("articulation iteration count exceeds u32");
    put(p, v);
  }
  for (const auto *p : {"SolverIterationCounts.minPositionIters",
                        "SolverIterationCounts.minVelocityIters"}) {
    const auto v = joint_detail::integer(o, p);
    if (!v || v > 255)
      throw ExportError("articulation solver iterations must be in [1,255]");
    put(p, v);
  }
  for (const auto *p : {"SeparationTolerance", "SleepThreshold",
                        "StabilizationThreshold", "WakeCounter"})
    put(p, joint_detail::nonnegative(o, p));
  if (const auto *name = o.property("Name")) {
    if (name->item_count != 1 || !name->text)
      throw ExportError("unresolved articulation Name");
    put("Name", *name->text);
  } else
    put("Name", std::string{});
  const auto roots = references(o, "Links");
  if (roots.size() != 1)
    throw ExportError("PVD articulation must publish exactly one root link");
  std::vector<std::pair<std::uint64_t, std::uint64_t>> pending{
      {roots[0], o.id}};
  std::set<std::uint64_t> visited, joints;
  for (std::size_t i = 0; i < pending.size(); ++i) {
    const auto id = pending[i].first, parent = pending[i].second;
    if (!visited.insert(id).second || visited.size() > 64)
      throw ExportError(
          "articulation graph repeats a link/cycles/exceeds 64 links");
    const auto &link = resolve(s, id);
    if (link.class_name != QualifiedName{"physx3", "PxArticulationLink"} ||
        reference(link, "Parent") != parent)
      throw ExportError("articulation Parent/Links graph disagrees");
    Link item;
    item.object = &link;
    item.parent = parent == o.id ? 0 : parent;
    const auto inbound = !item.parent && !link.property("InboundJoint")
                             ? 0
                             : reference(link, "InboundJoint");
    if (item.parent) {
      if (!inbound || !joints.insert(inbound).second)
        throw ExportError("missing/duplicate articulation inbound joint");
      item.joint = &resolve(s, inbound);
      if (reference(*item.joint, "Link") != id)
        throw ExportError("articulation inbound joint points to another link");
      item.joint_fields = joint_fields(*item.joint);
    } else if (inbound)
      throw ExportError("root articulation link has an inbound joint");
    result.links.push_back(std::move(item));
    for (auto child : references(link, "Links", true)) {
      if (pending.size() >= 64)
        throw ExportError("articulation graph exceeds 64 links");
      pending.emplace_back(child, id);
    }
  }
  if (visited != known_links)
    throw ExportError("articulation graph omits/orphans recorded links");
  return result;
}
inline Aggregate
read_aggregate(const Metadata &m, const Snapshot &s, const InstanceState &o,
               const std::map<std::uint64_t, Articulation> &articulations) {
  if (o.class_name != QualifiedName{"physx3", "PxAggregate"})
    throw ExportError("unsupported aggregate class");
  Aggregate result;
  result.object = &o;
  result.max_actors = joint_detail::integer(o, "MaxNbActors");
  const auto self = joint_detail::integer(o, "SelfCollision");
  if (result.max_actors > 128 || self > 1)
    throw ExportError("invalid aggregate capacity/self-collision");
  result.self_collision = self != 0;
  if (result.max_actors && !o.property("Actors") &&
      !o.property("Articulations"))
    throw ExportError("aggregate membership has not been published; empty "
                      "membership is unverified");
  result.actors = references(o, "Actors", true);
  result.articulations = references(o, "Articulations", true);
  result.actor_count = result.actors.size();
  for (auto id : result.actors) {
    const auto &actor = resolve(s, id);
    if (!(is(m, actor, "PxRigidStatic") || is(m, actor, "PxRigidDynamic")))
      throw ExportError("aggregate rigid member has unsupported class");
    if (actor.property("Aggregate")) {
      const auto back = reference(actor, "Aggregate");
      if (back && back != o.id)
        throw ExportError("aggregate rigid back-reference disagrees");
    }
  }
  for (auto id : result.articulations) {
    const auto found = articulations.find(id);
    if (found == articulations.end())
      throw ExportError(
          "aggregate articulation is absent/incomplete/outside selection");
    if (found->second.object->property("Aggregate")) {
      const auto back = reference(*found->second.object, "Aggregate");
      if (back && back != o.id)
        throw ExportError("aggregate articulation back-reference disagrees");
    }
    result.actor_count += found->second.links.size();
  }
  if (result.actor_count > result.max_actors)
    throw ExportError("aggregate members exceed capacity");
  return result;
}
} // namespace pvd::repx::articulation_detail
