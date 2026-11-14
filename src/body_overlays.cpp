#include "pvd/body_overlays.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace pvd::render {
namespace {
constexpr std::uint32_t bounds_color = 0xff68a6ccu, linear_color = 0xff58dba8u,
                        angular_color = 0xffe9ae57u;
bool finite(Vec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool valid_pose(const Transform &t) {
  const auto &q = t.rotation;
  const double n = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z +
                   double(q.w) * q.w;
  return finite(t.position) && std::isfinite(n) &&
         std::abs(std::sqrt(n) - 1) < 1e-4;
}
struct DVec {
  double x = 0, y = 0, z = 0;
};
DVec add(DVec a, DVec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
DVec sub(DVec a, DVec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
DVec mul(DVec a, double b) { return {a.x * b, a.y * b, a.z * b}; }
DVec cross(DVec a, DVec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(DVec a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
bool convert(DVec v, Vec3 &out) {
  const auto max = double(std::numeric_limits<float>::max());
  if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
      std::abs(v.x) > max || std::abs(v.y) > max || std::abs(v.z) > max)
    return false;
  out = {static_cast<float>(v.x), static_cast<float>(v.y),
         static_cast<float>(v.z)};
  return finite(out);
}
struct Builder {
  const Frame &frame;
  const Snapshot &snapshot;
  const BodyOverlayOptions &options;
  BodyOverlays output;
  std::size_t total_lines = 0;
  bool exhausted = false;
  std::set<PickingKey> visible, attachments;
  std::set<std::pair<ObjectKey, ObjectKey>> actors;
  void problem(BodyOverlayCode code, BodyOverlaySource source,
               const std::string &property, const std::string &text) {
    output.complete = false;
    if (output.diagnostics.size() < options.max_diagnostics)
      output.diagnostics.push_back({code, source, property, text});
  }
  bool room(std::size_t count, const BodyOverlaySource &source,
            const char *property) {
    if (count > options.max_lines - total_lines) {
      problem(
          BodyOverlayCode::limit_exceeded, source, property,
          "body overlay line budget exhausted before a complete AABB/arrow");
      exhausted = true;
      return false;
    }
    return true;
  }
  const InstanceState *object(ObjectKey key) const {
    if (!key.id)
      return nullptr;
    const auto *value = snapshot.instance(key.id);
    return value && value->generation == key.generation ? value : nullptr;
  }
  const InstanceState *attachment(const BodyOverlaySource &source) {
    const auto &key = source.key;
    const auto *actor = object(key.actor);
    if (!object(key.scene) || !actor || !object(key.shape)) {
      problem(BodyOverlayCode::stale_source, source, "attachment",
              "scene/actor/shape is absent or its generation differs from the "
              "render Frame");
      return nullptr;
    }
    if (const auto *scene = actor->property("Scene")) {
      std::optional<std::uint64_t> id;
      try {
        id = scene->as_object_ref();
      } catch (const std::out_of_range &) {
        problem(BodyOverlayCode::invalid_property, source, "Scene",
                "captured actor Scene reference has malformed bytes");
        return nullptr;
      }
      if (id && *id && *id != key.scene.id) {
        problem(BodyOverlayCode::stale_source, source, "Scene",
                "actor belongs to another captured scene");
        return nullptr;
      }
    }
    return actor;
  }
  void bounds(const ShapeInstance &shape, const BodyOverlaySource &source) {
    if (!shape.world_bounds) {
      problem(BodyOverlayCode::missing_bounds, source, "world_bounds",
              "attachment has no finite tight geometry AABB (infinite planes "
              "are unbounded)");
      return;
    }
    const auto &b = *shape.world_bounds;
    if (!finite(b.minimum) || !finite(b.maximum) || b.minimum.x > b.maximum.x ||
        b.minimum.y > b.maximum.y || b.minimum.z > b.maximum.z) {
      problem(
          BodyOverlayCode::invalid_bounds, source, "world_bounds",
          "tight geometry AABB must be finite and ordered minimum <= maximum");
      return;
    }
    if (!room(12, source, "world_bounds"))
      return;
    std::array<Vec3, 8> corners{};
    for (unsigned i = 0; i < 8; ++i)
      corners[i] = {(i & 1) ? b.maximum.x : b.minimum.x,
                    (i & 2) ? b.maximum.y : b.minimum.y,
                    (i & 4) ? b.maximum.z : b.minimum.z};
    std::array<BodyOverlayLine, 12> lines{};
    std::size_t count = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto bit = 1u << axis;
      for (unsigned i = 0; i < 8; ++i)
        if (!(i & bit))
          lines[count++] = {
              source,     BodyOverlayKind::bounds, BodyOverlayPart::bounds_edge,
              corners[i], corners[i | bit],        bounds_color,
              true};
    }
    output.bounds.insert(output.bounds.end(), lines.begin(), lines.end());
    total_lines += 12;
    ++output.bounds_count;
  }
  std::optional<Vec3> position(const InstanceState &actor,
                               const BodyOverlaySource &source) {
    const auto *p = actor.property("GlobalPose");
    if (!p) {
      problem(BodyOverlayCode::missing_property, source, "GlobalPose",
              "requested velocity display has no captured Actor.GlobalPose");
      return {};
    }
    if (p->item_count != 1 || p->raw_bytes().size() != 28 ||
        (p->byte_order != ByteOrder::little &&
         p->byte_order != ByteOrder::big)) {
      problem(BodyOverlayCode::invalid_property, source, "GlobalPose",
              "captured Actor.GlobalPose has malformed count, byte length or "
              "byte order");
      return {};
    }
    const auto pose = p->as_transform();
    if (!pose || p->item_count != 1 || !valid_pose(*pose)) {
      problem(BodyOverlayCode::invalid_property, source, "GlobalPose",
              "captured Actor.GlobalPose must be a finite typed transform with "
              "unit rotation");
      return {};
    }
    return pose->position;
  }
  void velocity(const InstanceState &actor, Vec3 anchor,
                const BodyOverlaySource &source, BodyOverlayKind kind) {
    const auto *name = kind == BodyOverlayKind::linear_velocity
                           ? "LinearVelocity"
                           : "AngularVelocity";
    const auto *p = actor.property(name);
    if (!p) {
      problem(BodyOverlayCode::missing_property, source, name,
              "requested body velocity was not captured");
      return;
    }
    if (p->item_count != 1 || p->raw_bytes().size() != 12 ||
        (p->byte_order != ByteOrder::little &&
         p->byte_order != ByteOrder::big)) {
      problem(BodyOverlayCode::invalid_property, source, name,
              "captured body velocity has malformed count, byte length or byte "
              "order");
      return;
    }
    const auto value = p->as_vec3();
    if (!value || p->item_count != 1 || !finite(*value)) {
      problem(BodyOverlayCode::invalid_property, source, name,
              "captured body velocity must be exactly one finite typed "
              "world-space PxVec3");
      return;
    }
    if (value->x == 0 && value->y == 0 && value->z == 0)
      return;
    const DVec start{anchor.x, anchor.y, anchor.z},
        delta{double(value->x) * options.velocity_scale,
              double(value->y) * options.velocity_scale,
              double(value->z) * options.velocity_scale};
    const double magnitude = length(delta);
    if (magnitude == 0)
      return;
    const auto direction = mul(delta, 1 / magnitude);
    const auto reference =
        std::abs(direction.x) < .8 ? DVec{1, 0, 0} : DVec{0, 1, 0};
    auto side = cross(direction, reference);
    side = mul(side, 1 / length(side));
    const auto endpoint = add(start, delta),
               base = sub(endpoint, mul(direction, magnitude * .15)),
               width = mul(side, magnitude * .075);
    Vec3 end, head0, head1;
    if (!convert(endpoint, end) || !convert(add(base, width), head0) ||
        !convert(sub(base, width), head1)) {
      problem(
          BodyOverlayCode::invalid_property, source, name,
          "derived velocity arrow overflows finite world-space coordinates");
      return;
    }
    // A tiny finite source can vanish at this world-space float precision.
    if (end.x == anchor.x && end.y == anchor.y && end.z == anchor.z &&
        head0.x == anchor.x && head0.y == anchor.y && head0.z == anchor.z &&
        head1.x == anchor.x && head1.y == anchor.y && head1.z == anchor.z)
      return;
    if (!room(3, source, name))
      return;
    const auto color =
        kind == BodyOverlayKind::linear_velocity ? linear_color : angular_color;
    const std::array<BodyOverlayLine, 3> lines{
        {{source, kind, BodyOverlayPart::vector_shaft, anchor, end, color,
          true},
         {source, kind, BodyOverlayPart::arrow_head, end, head0, color, true},
         {source, kind, BodyOverlayPart::arrow_head, end, head1, color, true}}};
    auto &destination = kind == BodyOverlayKind::linear_velocity
                            ? output.linear_velocity
                            : output.angular_velocity;
    destination.insert(destination.end(), lines.begin(), lines.end());
    total_lines += 3;
    if (kind == BodyOverlayKind::linear_velocity)
      ++output.linear_vectors;
    else
      ++output.angular_vectors;
  }
  BodyOverlays run() {
    const bool linear =
                   options.show_linear_velocity && options.velocity_scale > 0,
               angular =
                   options.show_angular_velocity && options.velocity_scale > 0;
    if ((!options.show_bounds && !linear && !angular) ||
        frame.instances.empty() ||
        (options.visible_attachments && options.visible_attachments->empty()))
      return std::move(output);
    if (frame.event_count != snapshot.event_count) {
      problem(BodyOverlayCode::snapshot_mismatch, {{}, snapshot.event_count},
              "event_count",
              "render Frame and Snapshot prefixes differ; overlays require one "
              "captured state");
      return std::move(output);
    }
    if (options.visible_attachments) {
      if (options.visible_attachments->size() > options.max_attachments) {
        problem(BodyOverlayCode::limit_exceeded, {{}, snapshot.event_count},
                "visible_attachments",
                "visible attachment mask exceeds work budget");
        return std::move(output);
      }
      visible.insert(options.visible_attachments->begin(),
                     options.visible_attachments->end());
    }
    for (const auto &shape : frame.instances) {
      const BodyOverlaySource source{shape.picking_key(), snapshot.event_count};
      if (output.processed_attachments >= options.max_attachments) {
        problem(BodyOverlayCode::limit_exceeded, source, "attachments",
                "body overlay attachment work budget exhausted");
        break;
      }
      ++output.processed_attachments;
      if (options.visible_attachments && !visible.count(source.key))
        continue;
      if (!attachments.insert(source.key).second)
        continue;
      const auto *actor = attachment(source);
      if (!actor)
        continue;
      if (options.show_bounds)
        bounds(shape, source);
      if (exhausted)
        break;
      if (!linear && !angular)
        continue;
      if (shape.actor.kind == ActorKind::rigid_static)
        continue;
      if (shape.actor.kind != ActorKind::rigid_dynamic &&
          shape.actor.kind != ActorKind::articulation_link) {
        problem(BodyOverlayCode::stale_source, source, "actor.kind",
                "unsupported actor kind in render Frame");
        continue;
      }
      if (!actors.insert({source.key.scene, source.key.actor}).second)
        continue;
      const auto anchor = position(*actor, source);
      if (!anchor)
        continue;
      if (linear)
        velocity(*actor, *anchor, source, BodyOverlayKind::linear_velocity);
      if (exhausted)
        break;
      if (angular)
        velocity(*actor, *anchor, source, BodyOverlayKind::angular_velocity);
      if (exhausted)
        break;
    }
    return std::move(output);
  }
};
} // namespace
BodyOverlays build_body_overlays(const Frame &frame, const Snapshot &snapshot,
                                 const BodyOverlayOptions &options) {
  if (!std::isfinite(options.velocity_scale) || options.velocity_scale < 0 ||
      !options.max_diagnostics)
    throw std::invalid_argument(
        "body overlay scale must be finite and nonnegative; diagnostic limit "
        "must be nonzero");
  Builder builder{frame, snapshot, options, {}, {}, false, {}, {}, {}};
  return builder.run();
}
} // namespace pvd::render
