#include "pvd/coordinate_frames.hpp"
#include "pvd/coordinates.hpp"
#include <cmath>
#include <set>
#include <stdexcept>

namespace pvd::render {
namespace {
bool finite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}
bool valid(Transform pose) {
  const auto &q = pose.rotation;
  const double norm = double(q.x) * q.x + double(q.y) * q.y +
                      double(q.z) * q.z + double(q.w) * q.w;
  return finite(pose.position) && std::isfinite(norm) &&
         std::abs(norm - 1) <= .001;
}
Transform mass_pose(Transform actor, Transform local) {
  const auto &a = actor.rotation;
  const auto &b = local.rotation;
  Transform result;
  result.position = transform_point(actor, local.position);
  result.rotation = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
  return result;
}
class Builder {
  const Frame &frame;
  const Snapshot &snapshot;
  const CoordinateFrameOptions &options;
  CoordinateFrames result;
  std::set<PickingKey> mask, seen_shapes;
  std::set<std::pair<ObjectKey, ObjectKey>> seen_actors;
  std::size_t lines = 0;
  void problem(CoordinateFrameCode code, CoordinateFrameSource source,
               const char *property, const char *message) {
    result.complete = false;
    if (result.diagnostics.size() < options.max_diagnostics)
      result.diagnostics.push_back({code, source, property, message});
  }
  bool source_valid(const ShapeInstance &shape, CoordinateFrameSource source) {
    const auto *scene = snapshot.instance(source.key.scene.id);
    const auto *actor = snapshot.instance(source.key.actor.id);
    const auto *object = snapshot.instance(source.key.shape.id);
    if (!scene || !actor || !object ||
        scene->generation != source.key.scene.generation ||
        actor->generation != source.key.actor.generation ||
        object->generation != source.key.shape.generation ||
        scene->class_name != QualifiedName{"physx3", "PxScene"} ||
        object->class_name != QualifiedName{"physx3", "PxShape"}) {
      problem(CoordinateFrameCode::stale_source, source, "attachment",
              "coordinate frame source is absent or has a different "
              "generation/class");
      return false;
    }
    if (shape.actor.kind != ActorKind::rigid_static &&
        shape.actor.kind != ActorKind::rigid_dynamic &&
        shape.actor.kind != ActorKind::articulation_link) {
      problem(CoordinateFrameCode::stale_source, source, "actor.kind",
              "unsupported captured actor kind for coordinate frames");
      return false;
    }
    const auto expected =
        shape.actor.kind == ActorKind::rigid_static    ? "PxRigidStatic"
        : shape.actor.kind == ActorKind::rigid_dynamic ? "PxRigidDynamic"
                                                       : "PxArticulationLink";
    if (actor->class_name != QualifiedName{"physx3", expected}) {
      problem(CoordinateFrameCode::stale_source, source, "actor.kind",
              "coordinate frame actor kind does not match its captured native "
              "class");
      return false;
    }
    if (const auto *value = actor->property("Scene")) {
      try {
        const auto actual = value->as_object_ref();
        if (actual && *actual && *actual != source.key.scene.id) {
          problem(CoordinateFrameCode::stale_source, source, "Scene",
                  "coordinate frame actor belongs to another captured scene");
          return false;
        }
      } catch (const std::out_of_range &) {
        problem(CoordinateFrameCode::invalid_property, source, "Scene",
                "captured actor Scene reference has malformed bytes");
        return false;
      }
    }
    return true;
  }
  std::optional<Transform> property(const InstanceState &actor,
                                    const char *name,
                                    CoordinateFrameSource source, bool native) {
    const auto *p = actor.property(name);
    if (!p) {
      problem(CoordinateFrameCode::missing_property, source, name,
              "requested coordinate frame property was not captured");
      return {};
    }
    const auto &bytes = native ? coordinates::native_bytes(*p) : p->raw_bytes();
    if (p->incoming_type != QualifiedName{"physx3", "PxTransform"} ||
        p->item_count != 1 || bytes.size() != 28) {
      problem(
          CoordinateFrameCode::invalid_property, source, name,
          "coordinate frame requires exactly one captured typed PxTransform");
      return {};
    }
    auto pose = native ? coordinates::native_transform(*p) : p->as_transform();
    if (!pose || !valid(*pose)) {
      problem(
          CoordinateFrameCode::invalid_property, source, name,
          "coordinate frame pose must be finite with a normalized quaternion");
      return {};
    }
    return pose;
  }
  void triad(Transform pose, CoordinateFrameSource source) {
    if (lines > options.max_lines || options.max_lines - lines < 3) {
      problem(CoordinateFrameCode::limit_exceeded, source, "max_lines",
              "coordinate frame line budget cannot fit a complete three-axis "
              "triad");
      return;
    }
    if (!valid(pose)) {
      problem(CoordinateFrameCode::invalid_property, source, "world_pose",
              "requested coordinate frame has an invalid derived world pose");
      return;
    }
    const Vec3 axes[] = {{options.axis_length, 0, 0},
                         {0, options.axis_length, 0},
                         {0, 0, options.axis_length}};
    const std::uint32_t colors[] = {0xffec6558u, 0xff65d780u, 0xff669cecu};
    std::vector<CoordinateFrameLine> ready;
    ready.reserve(3);
    for (unsigned axis = 0; axis < 3; ++axis) {
      auto end = transform_point(pose, axes[axis]);
      if (!finite(end)) {
        problem(
            CoordinateFrameCode::invalid_property, source, "axis_length",
            "derived coordinate frame axis exceeds finite world coordinates");
        return;
      }
      ready.push_back({source, axis, pose.position, end, colors[axis]});
    }
    auto &destination = source.kind == CoordinateFrameKind::actor ? result.actor
                        : source.kind == CoordinateFrameKind::shape
                            ? result.shape
                            : result.mass;
    destination.insert(destination.end(), ready.begin(), ready.end());
    lines += 3;
    if (source.kind == CoordinateFrameKind::actor)
      ++result.actor_count;
    else if (source.kind == CoordinateFrameKind::shape)
      ++result.shape_count;
    else
      ++result.mass_count;
  }

public:
  Builder(const Frame &f, const Snapshot &s, const CoordinateFrameOptions &o)
      : frame(f), snapshot(s), options(o) {}
  CoordinateFrames run() {
    if ((!options.actor && !options.shape && !options.mass) ||
        options.axis_length == 0 ||
        (options.attachments && options.attachments->empty()))
      return std::move(result);
    if (frame.event_count != snapshot.event_count) {
      problem(
          CoordinateFrameCode::snapshot_mismatch, {}, "event_count",
          "coordinate frames require Frame and Snapshot at the same prefix");
      return std::move(result);
    }
    if (options.attachments) {
      if (options.attachments->size() > options.max_attachments) {
        problem(CoordinateFrameCode::limit_exceeded, {}, "attachments",
                "coordinate frame attachment mask exceeds its work budget");
        return std::move(result);
      }
      mask.insert(options.attachments->begin(), options.attachments->end());
    }
    for (const auto &shape : frame.instances) {
      if (result.processed_attachments >= options.max_attachments) {
        problem(CoordinateFrameCode::limit_exceeded, {}, "max_attachments",
                "coordinate frame attachment work budget exhausted");
        break;
      }
      ++result.processed_attachments;
      CoordinateFrameSource source{shape.picking_key(), snapshot.event_count,
                                   CoordinateFrameKind::shape};
      if (options.attachments && !mask.count(source.key))
        continue;
      if (!seen_shapes.insert(source.key).second ||
          !source_valid(shape, source))
        continue;
      if (options.shape)
        triad(shape.world_pose, source);
      if (!seen_actors.insert({source.key.scene, source.key.actor}).second)
        continue;
      const auto *actor = snapshot.instance(source.key.actor.id);
      if (!options.actor &&
          (!options.mass || shape.actor.kind == ActorKind::rigid_static))
        continue;
      source.kind = CoordinateFrameKind::actor;
      const auto global = property(*actor, "GlobalPose", source, true);
      if (!global)
        continue;
      if (options.actor)
        triad(*global, source);
      if (options.mass && shape.actor.kind != ActorKind::rigid_static) {
        source.kind = CoordinateFrameKind::mass;
        if (auto local = property(*actor, "CMassLocalPose", source, false))
          triad(mass_pose(*global, *local), source);
      }
    }
    return std::move(result);
  }
};
} // namespace
CoordinateFrames
build_coordinate_frames(const Frame &frame, const Snapshot &snapshot,
                        const CoordinateFrameOptions &options) {
  if (!std::isfinite(options.axis_length) || options.axis_length < 0 ||
      options.axis_length > 1e6f)
    throw std::invalid_argument(
        "coordinate frame axis length must be finite in [0,1000000]");
  return Builder(frame, snapshot, options).run();
}
} // namespace pvd::render
