#include "pvd/repx.hpp"
#include "pvd/render.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <ostream>
#include <set>
#include <system_error>
#include <type_traits>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pvd::repx {
namespace {
ObjectKey key(const InstanceState &o) { return {o.id, o.generation}; }
std::string context(const InstanceState &o, const std::string &p) {
  return o.class_name.str() + " #" + std::to_string(o.id) + "." + p + ": ";
}
const PropertyValue &need(const InstanceState &o, const std::string &p) {
  const auto *v = o.property(p);
  if (!v)
    throw ExportError(context(o, p) + "missing recorded property");
  return *v;
}
std::uint64_t integer(const InstanceState &o, const std::string &p) {
  auto v = need(o, p).as_uint();
  if (!v)
    throw ExportError(context(o, p) + "expected scalar integer");
  return *v;
}
float scalar(const InstanceState &o, const std::string &p) {
  auto v = need(o, p).as_float();
  if (!v || !std::isfinite(*v))
    throw ExportError(context(o, p) + "expected finite scalar float");
  return *v;
}
Vec3 vector(const InstanceState &o, const std::string &p) {
  auto v = need(o, p).as_vec3();
  if (!v || !std::isfinite(v->x) || !std::isfinite(v->y) ||
      !std::isfinite(v->z))
    throw ExportError(context(o, p) + "expected finite Vec3");
  return *v;
}
Transform pose(const InstanceState &o, const std::string &p) {
  auto v = need(o, p).as_transform();
  if (!v)
    throw ExportError(context(o, p) + "expected transform");
  const auto &q = v->rotation;
  const auto n = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z +
                 double(q.w) * q.w;
  if (!std::isfinite(n) || std::abs(n - 1) > 1e-3 ||
      !std::isfinite(v->position.x) || !std::isfinite(v->position.y) ||
      !std::isfinite(v->position.z))
    throw ExportError(context(o, p) + "invalid transform");
  return *v;
}
std::vector<std::uint64_t> references(const InstanceState &o,
                                      const std::string &p) {
  const auto &value = need(o, p);
  std::vector<std::uint64_t> result;
  if (value.item_count > std::numeric_limits<std::size_t>::max())
    throw ExportError(context(o, p) + "reference count overflow");
  result.reserve(static_cast<std::size_t>(value.item_count));
  for (std::size_t i = 0; i < value.item_count; ++i) {
    auto id = value.as_object_ref(i);
    if (!id)
      throw ExportError(context(o, p) + "unresolved object-reference encoding");
    result.push_back(*id);
  }
  return result;
}
std::string number(float value) {
  if (!std::isfinite(value))
    throw ExportError("non-finite RepX scalar");
  char buffer[64];
  const auto r = std::to_chars(buffer, buffer + sizeof(buffer), value,
                               std::chars_format::general,
                               std::numeric_limits<float>::max_digits10);
  if (r.ec != std::errc{})
    throw ExportError("float formatting failed");
  return std::string(buffer, r.ptr);
}
std::string vec(Vec3 v) {
  return number(v.x) + " " + number(v.y) + " " + number(v.z);
}
std::string quat(Quat q) {
  return number(q.x) + " " + number(q.y) + " " + number(q.z) + " " +
         number(q.w);
}
std::string transform(Transform t) {
  return quat(t.rotation) + " " + vec(t.position);
}
std::string escape(const std::string &s) {
  // XML 1.0 Unicode validation, including malformed UTF-8 and C0 controls.
  for (std::size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    std::uint32_t cp = 0;
    std::size_t n = 1;
    if (b < 0x80)
      cp = b;
    else if (b >= 0xc2 && b <= 0xdf) {
      cp = b & 31;
      n = 2;
    } else if (b >= 0xe0 && b <= 0xef) {
      cp = b & 15;
      n = 3;
    } else if (b >= 0xf0 && b <= 0xf4) {
      cp = b & 7;
      n = 4;
    } else
      throw ExportError("invalid UTF-8 in XML text");
    if (n > s.size() - i)
      throw ExportError("truncated UTF-8 in XML text");
    for (std::size_t j = 1; j < n; ++j) {
      const auto c = static_cast<unsigned char>(s[i + j]);
      if ((c & 0xc0) != 0x80)
        throw ExportError("invalid UTF-8 continuation in XML text");
      cp = (cp << 6) | (c & 63);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
        (n == 4 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff) ||
        !(cp == 9 || cp == 10 || cp == 13 || (cp >= 0x20 && cp <= 0xd7ff) ||
          (cp >= 0xe000 && cp <= 0xfffd) || (cp >= 0x10000 && cp <= 0x10ffff)))
      throw ExportError("character is not valid XML 1.0 text");
    i += n;
  }
  std::string out;
  const bool boundary_space =
      !s.empty() && (s.front() == ' ' || s.back() == ' ');
  for (char c : s) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '\"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    case '\r':
      out += "&#x0D;";
      break;
    case '\n':
      out += "&#x0A;";
      break;
    case '\t':
      out += "&#x09;";
      break;
    case ' ':
      out += boundary_space ? "&#x20;" : " ";
      break;
    default:
      out += c;
    }
  }
  return out;
}
std::string text(const InstanceState &o, const char *property) {
  const auto *v = o.property(property);
  if (!v)
    return {};
  if (!v->text)
    throw ExportError(context(o, property) + "unresolved string");
  (void)escape(*v->text);
  return *v->text;
}
std::string flags(std::uint64_t value,
                  std::initializer_list<const char *> names) {
  std::string out;
  std::uint64_t bit = 1;
  for (const auto *name : names) {
    if (value & bit) {
      if (!out.empty())
        out += '|';
      out += name;
      value &= ~bit;
    }
    bit <<= 1;
  }
  if (value)
    throw ExportError("unrecognized PhysX 3.4 flag bits");
  return out;
}
std::string actor_flags(std::uint64_t v) {
  return flags(v, {"eVISUALIZATION", "eDISABLE_GRAVITY", "eSEND_SLEEP_NOTIFIES",
                   "eDISABLE_SIMULATION"});
}
std::string body_flags(std::uint64_t v) {
  return flags(v,
               {"eKINEMATIC", "eUSE_KINEMATIC_TARGET_FOR_SCENE_QUERIES",
                "eENABLE_CCD", "eENABLE_CCD_FRICTION",
                "eENABLE_POSE_INTEGRATION_PREVIEW", "eENABLE_SPECULATIVE_CCD",
                "eENABLE_CCD_MAX_CONTACT_IMPULSE"});
}
std::string shape_flags(std::uint64_t v) {
  return flags(v, {"eSIMULATION_SHAPE", "eSCENE_QUERY_SHAPE", "eTRIGGER_SHAPE",
                   "eVISUALIZATION"});
}
std::string lock_flags(std::uint64_t v) {
  return flags(v, {"eLOCK_LINEAR_X", "eLOCK_LINEAR_Y", "eLOCK_LINEAR_Z",
                   "eLOCK_ANGULAR_X", "eLOCK_ANGULAR_Y", "eLOCK_ANGULAR_Z"});
}
std::string combine(std::uint64_t value) {
  static const char *names[] = {"eAVERAGE", "eMIN", "eMULTIPLY", "eMAX"};
  if (value >= 4)
    throw ExportError("unknown material combine mode");
  return names[value];
}
std::string mesh_flags(std::uint64_t value) {
  if (value & ~std::uint64_t(2))
    throw ExportError("unknown triangle/heightfield geometry flag bits");
  return value ? "eDOUBLE_SIDED" : "";
}
struct Xml {
  std::string data;
  std::size_t limit;
  const std::function<bool()> &cancel;
  void check() {
    if (cancel && cancel())
      throw ExportCancelled{};
  }
  void add(const std::string &s) {
    check();
    if (s.size() > limit - data.size())
      throw ExportError("RepX XML output budget exceeded");
    data += s;
  }
  void open(const std::string &tag) { add("<" + tag + ">\n"); }
  void close(const std::string &tag) { add("</" + tag + ">\n"); }
  void element(const std::string &tag, const std::string &value) {
    add("<" + tag + ">" + escape(value) + "</" + tag + ">\n");
  }
};
struct Shape {
  const InstanceState *object;
  const render::ShapeInstance *extracted;
  std::vector<std::uint64_t> materials;
  bool exclusive = false;
};
struct Actor {
  const InstanceState *object;
  std::vector<std::uint64_t> shapes;
  bool dynamic = false;
};
struct Resource {
  const InstanceState *object;
  std::shared_ptr<const render::MeshData> mesh;
  render::GeometryKind kind;
};
struct Build {
  const Metadata &metadata;
  const Snapshot &snapshot;
  const Options &options;
  Result result;
  std::map<std::uint64_t, Actor> actors;
  std::map<std::uint64_t, Shape> shapes;
  std::map<std::uint64_t, const InstanceState *> materials;
  std::map<std::uint64_t, Resource> resources;
  std::map<std::uint64_t, std::uint64_t> ids;
  std::set<std::uint64_t> selected;
  std::map<std::uint64_t, std::set<std::uint64_t>> selected_attachments;
  Build(const Metadata &m, const Snapshot &s, const Options &o)
      : metadata(m), snapshot(s), options(o) {}
  std::vector<std::uint64_t> actor_shapes(const InstanceState &actor) const {
    auto shape_refs = references(actor, "Shapes");
    if (options.selection == Selection::attachments) {
      const auto found = selected_attachments.find(actor.id);
      shape_refs.erase(std::remove_if(shape_refs.begin(), shape_refs.end(),
                                      [&](auto id) {
                                        return found ==
                                                   selected_attachments.end() ||
                                               !found->second.count(id);
                                      }),
                       shape_refs.end());
    }
    return shape_refs;
  }
  bool is(const InstanceState &o, const char *type) const {
    return metadata.is_a(o.class_name, {"physx3", type});
  }
  void check() const {
    if (options.cancelled && options.cancelled())
      throw ExportCancelled{};
  }
  bool in_scene(const InstanceState &actor) const {
    if (!options.scene)
      return true;
    const auto *p = actor.property("Scene");
    auto id = p ? p->as_object_ref() : std::nullopt;
    const auto *scene = id ? snapshot.instance(*id) : nullptr;
    return scene && scene->id == options.scene->id &&
           scene->generation == options.scene->generation;
  }
  void issue(const InstanceState &o, const std::string &property,
             const std::string &message) {
    if (options.unsupported == UnsupportedPolicy::reject)
      throw ExportError(context(o, property) + message);
    result.complete = false;
    result.diagnostics.push_back({Severity::error, key(o), property, message});
  }
  const InstanceState &resolve(std::uint64_t id) const {
    const auto *o = snapshot.instance(id);
    if (!o)
      throw ExportError("dangling RepX dependency #" + std::to_string(id));
    return *o;
  }
  void select() {
    if (options.scene) {
      const auto *scene = snapshot.instance(options.scene->id);
      if (!scene || scene->generation != options.scene->generation ||
          !is(*scene, "PxScene"))
        throw ExportError("selected RepX scene is absent/stale");
    }
    if (options.selection == Selection::all_actors) {
      for (const auto &entry : snapshot.instances) {
        check();
        const auto &o = entry.second;
        const bool unknown_top =
            o.top_level && !is(o, "PxActor") &&
            !(is(o, "PxPhysics") || is(o, "PxScene") || is(o, "PxShape") ||
              is(o, "PxMaterial") || is(o, "PxGeometry") ||
              is(o, "PxTriangleMesh") || is(o, "PxConvexMesh") ||
              is(o, "PxHeightField") ||
              o.class_name.namespace_name == "_debugger_");
        if (unknown_top) {
          if (!options.scene || !o.property("Scene") || in_scene(o))
            issue(o, "class",
                  "unknown top-level class cannot be declared a complete RepX "
                  "actor export");
          continue;
        }
        if (options.scene && is(o, "PxActor") && !o.property("Scene")) {
          issue(o, "Scene",
                "cannot determine scene membership for this "
                "unsupported/detached actor");
          continue;
        }
        if (!in_scene(o))
          continue;
        if (is(o, "PxRigidStatic") || is(o, "PxRigidDynamic"))
          selected.insert(o.id);
        else if (is(o, "PxActor"))
          issue(o, "class",
                "unsupported actor: articulation/cloth/particles/APEX require "
                "dedicated serialization");
      }
    } else if (options.selection == Selection::attachments) {
      if (options.attachments.size() > options.max_objects)
        throw ExportError("RepX attachment selection budget exceeded");
      for (const auto &requested : options.attachments) {
        check();
        const auto &actor = resolve(requested.actor.id);
        const auto &shape = resolve(requested.shape.id);
        const auto &scene = resolve(requested.scene.id);
        if (actor.generation != requested.actor.generation ||
            shape.generation != requested.shape.generation ||
            scene.generation != requested.scene.generation)
          throw ExportError("requested RepX attachment is stale");
        if (!is(scene, "PxScene") || !is(shape, "PxShape"))
          throw ExportError(
              "requested RepX attachment has invalid scene/shape type");
        if (!(is(actor, "PxRigidStatic") || is(actor, "PxRigidDynamic"))) {
          issue(actor, "class", "unsupported actor in requested attachment");
          continue;
        }
        const auto source_scene = need(actor, "Scene").as_object_ref();
        if (!source_scene || *source_scene != scene.id || !in_scene(actor))
          throw ExportError(
              "requested RepX attachment scene does not own this actor");
        const auto source_shapes = references(actor, "Shapes");
        if (std::find(source_shapes.begin(), source_shapes.end(), shape.id) ==
            source_shapes.end())
          throw ExportError(
              "requested RepX shape is not attached to this actor");
        selected.insert(actor.id);
        selected_attachments[actor.id].insert(shape.id);
      }
    } else
      for (const auto &requested : options.objects) {
        check();
        const auto *o = snapshot.instance(requested.id);
        if (!o || o->generation != requested.generation)
          throw ExportError("requested RepX object is absent/stale: " +
                            std::to_string(requested.id));
        if (is(*o, "PxRigidStatic") || is(*o, "PxRigidDynamic")) {
          if (!in_scene(*o))
            throw ExportError("requested actor is outside selected scene");
          selected.insert(o->id);
        } else if (is(*o, "PxShape")) {
          bool owner = false;
          for (const auto &entry : snapshot.instances) {
            const auto &actor = entry.second;
            if (!(is(actor, "PxRigidStatic") || is(actor, "PxRigidDynamic")) ||
                !in_scene(actor))
              continue;
            auto refs = references(actor, "Shapes");
            if (std::find(refs.begin(), refs.end(), o->id) != refs.end()) {
              selected.insert(actor.id);
              owner = true;
            }
          }
          if (!owner)
            issue(*o, "Actor", "selected shape has no supported owning actor");
        } else
          issue(*o, "class",
                "selection roots must be supported rigid actors or attached "
                "shapes");
      }
    // Constraints/joints are not silently detached from the selected actors.
    for (const auto &entry : snapshot.instances) {
      const auto &o = entry.second;
      if (!(is(o, "PxJoint") || is(o, "PxConstraint") ||
            is(o, "PxArticulation") || is(o, "PxAggregate")))
        continue;
      bool relevant =
          options.selection == Selection::all_actors && !options.scene;
      for (const auto &p : o.properties)
        if (p.second.reference_ids)
          for (auto id : *p.second.reference_ids)
            relevant = relevant || selected.count(id);
      if (relevant)
        issue(o, "class",
              "joint/articulation/aggregate relationships are not implemented");
    }
    if (selected.empty())
      throw ExportError("no supported actors in requested RepX selection");
  }
  void validate_material(const InstanceState &m) {
    if (!is(m, "PxMaterial"))
      throw ExportError("shape material dependency is not PxMaterial");
    for (const auto *p : {"DynamicFriction", "StaticFriction", "Restitution"}) {
      auto v = scalar(m, p);
      if (v < 0 || (std::string(p) == "Restitution" && v > 1))
        throw ExportError(context(m, p) + "invalid material scalar");
    }
    flags(integer(m, "Flags"),
          {"eDISABLE_FRICTION", "eDISABLE_STRONG_FRICTION"});
    combine(integer(m, "FrictionCombineMode"));
    combine(integer(m, "RestitutionCombineMode"));
  }
  Snapshot geometry_input() {
    std::set<std::uint64_t> mesh_ids;
    std::size_t vertices = 0, triangles = 0, cells = 0;
    auto add = [&](std::size_t &count, std::uint64_t amount,
                   std::size_t limit) {
      if (amount > limit || count > limit - static_cast<std::size_t>(amount))
        throw ExportError("RepX total geometry input budget exceeded");
      count += static_cast<std::size_t>(amount);
    };
    for (auto actor_id : selected) {
      check();
      for (auto shape_id : actor_shapes(resolve(actor_id))) {
        const auto &shape = resolve(shape_id);
        const auto geometry_id = need(shape, "Geometry").as_object_ref();
        if (!geometry_id)
          throw ExportError("unresolved shape geometry reference");
        const auto &geometry = resolve(*geometry_id);
        const char *property =
            is(geometry, "PxConvexMeshGeometry")     ? "ConvexMesh"
            : is(geometry, "PxTriangleMeshGeometry") ? "TriangleMesh"
            : is(geometry, "PxHeightFieldGeometry")  ? "HeightField"
                                                     : nullptr;
        if (!property)
          continue;
        const auto resource_id = need(geometry, property).as_object_ref();
        if (!resource_id)
          throw ExportError("unresolved geometry resource reference");
        if (!mesh_ids.insert(*resource_id).second)
          continue;
        const auto &resource = resolve(*resource_id);
        if (is(resource, "PxHeightField")) {
          const auto rows = integer(resource, "NbRows"),
                     columns = integer(resource, "NbColumns");
          if (columns &&
              rows > std::numeric_limits<std::uint64_t>::max() / columns)
            throw ExportError("heightfield cell overflow");
          add(cells, rows * columns, options.max_heightfield_samples);
          add(vertices, rows * columns, options.max_vertices);
          if (rows && columns) {
            if (rows - 1 > std::numeric_limits<std::uint64_t>::max() /
                               (columns - 1 ? columns - 1 : 1) / 2)
              throw ExportError("heightfield triangle overflow");
            add(triangles, (rows - 1) * (columns - 1) * 2,
                options.max_triangles);
          }
        } else {
          add(vertices, need(resource, "Points").item_count,
              options.max_vertices);
          if (is(resource, "PxTriangleMesh"))
            add(triangles, need(resource, "Triangles").raw_bytes().size() / 6,
                options.max_triangles);
          else if (resource.property("PolygonIndexes"))
            add(triangles, need(resource, "PolygonIndexes").item_count,
                options.max_triangles);
        }
      }
    }
    Snapshot filtered = snapshot;
    for (auto it = filtered.instances.begin(); it != filtered.instances.end();)
      if (is(it->second, "PxActor") && !selected.count(it->first))
        it = filtered.instances.erase(it);
      else
        ++it;
    if (options.selection == Selection::attachments)
      for (auto id : selected) {
        auto &value = filtered.instances.at(id).properties.at("Shapes");
        const auto shape_refs = actor_shapes(resolve(id));
        if (value.pointer_width != 4 && value.pointer_width != 8)
          throw ExportError(
              "invalid source pointer width for attachment filtering");
        Bytes bytes;
        for (auto shape : shape_refs)
          for (std::size_t i = 0; i < value.pointer_width; ++i)
            bytes.push_back(static_cast<std::uint8_t>(
                shape >> ((value.byte_order == ByteOrder::little
                               ? i
                               : value.pointer_width - i - 1) *
                          8)));
        value.reference_ids = shape_refs;
        value.item_count = shape_refs.size();
        value.bytes = std::make_shared<const Bytes>(std::move(bytes));
      }
    return filtered;
  }
  void validate_actor(const InstanceState &o, bool dynamic) {
    pose(o, "GlobalPose");
    text(o, "Name");
    actor_flags(integer(o, "ActorFlags"));
    if (integer(o, "DominanceGroup") > 31 || integer(o, "OwnerClient") > 127)
      throw ExportError("actor dominance/client index out of range");
    if (o.property("ClientBehaviorFlags"))
      flags(integer(o, "ClientBehaviorFlags"),
            {"eREPORT_TO_FOREIGN_CLIENTS_TRIGGER_NOTIFY",
             "eREPORT_TO_FOREIGN_CLIENTS_CONTACT_NOTIFY",
             "eREPORT_TO_FOREIGN_CLIENTS_CONSTRAINT_BREAK_NOTIFY",
             "eREPORT_TO_FOREIGN_CLIENTS_SCENE_QUERY"});
    if (!dynamic)
      return;
    pose(o, "CMassLocalPose");
    scalar(o, "Mass");
    vector(o, "MassSpaceInertiaTensor");
    const auto linear = vector(o, "LinearVelocity"),
               angular = vector(o, "AngularVelocity");
    const auto body = integer(o, "RigidBodyFlags");
    body_flags(body);
    if ((body & 1) &&
        (linear.x || linear.y || linear.z || angular.x || angular.y ||
         angular.z || scalar(o, "WakeCounter") != 0 ||
         o.property("KinematicTarget")))
      issue(o, "LinearVelocity/AngularVelocity",
            "SDK RepX loader does not restore kinematic velocity/wake/target "
            "state; reloaded velocities and wake counter are zero and no "
            "pending target is restored (explicit partial export only)");
    if (o.property("KinematicTarget"))
      pose(o, "KinematicTarget");
    if (o.property("RigidDynamicLockFlags"))
      lock_flags(integer(o, "RigidDynamicLockFlags"));
    for (const auto *p :
         {"MinCCDAdvanceCoefficient", "MaxDepenetrationVelocity",
          "MaxContactImpulse", "LinearDamping", "AngularDamping",
          "MaxAngularVelocity", "SleepThreshold", "StabilizationThreshold",
          "WakeCounter", "ContactReportThreshold"})
      scalar(o, p);
    integer(o, "SolverIterationCounts.minPositionIters");
    integer(o, "SolverIterationCounts.minVelocityIters");
  }
  void prepare(render::Frame &frame) {
    std::map<std::pair<std::uint64_t, std::uint64_t>,
             const render::ShapeInstance *>
        extracted;
    for (const auto &shape : frame.instances)
      extracted[{shape.actor_id, shape.shape_id}] = &shape;
    for (auto id : selected) {
      check();
      const auto &o = resolve(id);
      try {
        const bool dynamic = is(o, "PxRigidDynamic");
        validate_actor(o, dynamic);
        auto refs = actor_shapes(o);
        if (std::set<std::uint64_t>(refs.begin(), refs.end()).size() !=
            refs.size())
          throw ExportError("actor repeats the same shape reference");
        std::map<std::uint64_t, Shape> pending;
        for (auto shape_id : refs) {
          const auto &s = resolve(shape_id);
          auto found = extracted.find({id, shape_id});
          if (!is(s, "PxShape") || found == extracted.end())
            throw ExportError("shape geometry missing/invalid: " +
                              std::to_string(shape_id));
          pose(s, "LocalPose");
          text(s, "Name");
          shape_flags(integer(s, "Flags"));
          scalar(s, "ContactOffset");
          scalar(s, "RestOffset");
          for (const auto *p : {"SimulationFilterData", "QueryFilterData"}) {
            const auto &value = need(s, p);
            if (value.raw_bytes().size() != 16)
              throw ExportError(context(s, p) +
                                "expected four u32 filter words");
          }
          auto mats = references(s, "Materials");
          if (mats.empty() || mats.size() > 128)
            throw ExportError("shape material count is invalid");
          for (auto material : mats)
            validate_material(resolve(material));
          if (std::holds_alternative<render::TriangleMesh>(
                  found->second->geometry) &&
              mats.size() > 1 &&
              std::get<render::TriangleMesh>(found->second->geometry)
                  .mesh->material_indices.empty())
            throw ExportError("PVD omitted per-triangle material indices for a "
                              "multi-material mesh");
          pending.emplace(shape_id, Shape{&s, found->second, std::move(mats),
                                          integer(s, "IsExclusive") != 0});
        }
        actors.emplace(id, Actor{&o, std::move(refs), dynamic});
        shapes.insert(pending.begin(), pending.end());
      } catch (const ExportError &e) {
        issue(o, "export", e.what());
      }
    }
    if (actors.empty())
      throw ExportError("no actors remain after RepX validation");
    std::map<std::uint64_t, std::size_t> owners;
    for (const auto &actor : actors)
      for (auto id : actor.second.shapes)
        if (++owners[id] > 1 && shapes.at(id).exclusive)
          throw ExportError(
              "exclusive shape has multiple owners in source snapshot");
    for (const auto &entry : shapes) {
      const auto &s = entry.second;
      for (auto id : s.materials)
        materials.emplace(id, &resolve(id));
      std::visit(
          [&](const auto &g) {
            using T = std::decay_t<decltype(g)>;
            if constexpr (std::is_same_v<T, render::ConvexMesh> ||
                          std::is_same_v<T, render::TriangleMesh> ||
                          std::is_same_v<T, render::HeightField>) {
              if (!g.mesh)
                throw ExportError("missing mesh resource");
              resources.emplace(
                  g.mesh->resource.id,
                  Resource{&resolve(g.mesh->resource.id), g.mesh,
                           render::geometry_kind(s.extracted->geometry)});
            }
          },
          s.extracted->geometry);
    }
    for (const auto &entry : resources)
      if (entry.second.kind == render::GeometryKind::convex_mesh &&
          entry.second.mesh->vertices.size() > 255)
        throw ExportError(
            "convex mesh exceeds SDK RepX fallback cook vertexLimit=255; exact "
            "geometry reconstruction cannot be guaranteed");
    for (const auto &entry : resources)
      if (entry.second.kind == render::GeometryKind::height_field) {
        const auto &o = *entry.second.object;
        if (integer(o, "Format") != 1)
          throw ExportError("unsupported heightfield sample format");
        scalar(o, "Thickness");
        scalar(o, "ConvexEdgeThreshold");
        flags(integer(o, "Flags"), {"eNO_BOUNDARY_EDGES"});
        const auto &samples = need(o, "Samples");
        const auto cells = integer(o, "NbRows") * integer(o, "NbColumns");
        if (cells > options.max_heightfield_samples ||
            samples.item_count != cells ||
            samples.raw_bytes().size() != cells * 4)
          throw ExportError("heightfield sample count/size invalid");
      }
    if (actors.size() + shapes.size() + materials.size() + resources.size() >
        options.max_objects)
      throw ExportError("RepX dependency object budget exceeded");
  }
  std::uint64_t serial(std::uint64_t id) const {
    auto found = ids.find(id);
    if (found == ids.end())
      throw ExportError("missing RepX serial ID");
    return found->second;
  }
  void allocate_ids() {
    std::uint64_t n = 1;
    auto add = [&](const InstanceState &o, const char *type) {
      ids.emplace(o.id, n);
      result.objects.push_back({key(o), n++, type, 0, 0});
    };
    for (const auto &m : materials)
      add(*m.second, "PxMaterial");
    for (const auto &resource : resources)
      add(*resource.second.object,
          resource.second.kind == render::GeometryKind::height_field
              ? "PxHeightField"
          : resource.second.kind == render::GeometryKind::convex_mesh
              ? "PxConvexMesh"
              : "PxBVH33TriangleMesh");
    for (const auto &shape : shapes)
      if (!shape.second.exclusive)
        add(*shape.second.object, "PxShape");
    for (const auto &actor : actors)
      add(*actor.second.object,
          actor.second.dynamic ? "PxRigidDynamic" : "PxRigidStatic");
    for (const auto &actor : actors)
      for (std::size_t i = 0; i < actor.second.shapes.size(); ++i) {
        const auto &shape = shapes.at(actor.second.shapes[i]);
        if (shape.exclusive)
          result.objects.push_back(
              {key(*shape.object), 0, "PxShape", serial(actor.first), i});
      }
  }
  void geometry(Xml &xml, const render::Geometry &geometry) {
    xml.open("Geometry");
    std::visit(
        [&](const auto &g) {
          using T = std::decay_t<decltype(g)>;
          if constexpr (std::is_same_v<T, render::Box>) {
            xml.open("PxBoxGeometry");
            xml.element("HalfExtents", vec(g.half_extents));
            xml.close("PxBoxGeometry");
          } else if constexpr (std::is_same_v<T, render::Sphere>) {
            xml.open("PxSphereGeometry");
            xml.element("Radius", number(g.radius));
            xml.close("PxSphereGeometry");
          } else if constexpr (std::is_same_v<T, render::Capsule>) {
            xml.open("PxCapsuleGeometry");
            xml.element("Radius", number(g.radius));
            xml.element("HalfHeight", number(g.half_height));
            xml.close("PxCapsuleGeometry");
          } else if constexpr (std::is_same_v<T, render::Plane>) {
            xml.open("PxPlaneGeometry");
            xml.close("PxPlaneGeometry");
          } else if constexpr (std::is_same_v<T, render::HeightField>) {
            xml.open("PxHeightFieldGeometry");
            xml.element("HeightField",
                        std::to_string(serial(g.mesh->resource.id)));
            xml.element("HeightScale", number(g.height_scale));
            xml.element("RowScale", number(g.row_scale));
            xml.element("ColumnScale", number(g.column_scale));
            xml.element("HeightFieldFlags", mesh_flags(g.flags));
            xml.close("PxHeightFieldGeometry");
          } else {
            const bool convex = std::is_same_v<T, render::ConvexMesh>;
            const char *tag =
                convex ? "PxConvexMeshGeometry" : "PxTriangleMeshGeometry";
            xml.open(tag);
            xml.open("Scale");
            xml.element("Scale", vec(g.scale.scale));
            xml.element("Rotation", quat(g.scale.rotation));
            xml.close("Scale");
            xml.element("MeshFlags",
                        convex ? flags(g.mesh_flags, {"eTIGHT_BOUNDS"})
                               : mesh_flags(g.mesh_flags));
            xml.element(convex ? "ConvexMesh" : "TriangleMesh",
                        std::to_string(serial(g.mesh->resource.id)));
            xml.close(tag);
          }
        },
        geometry);
    xml.close("Geometry");
  }
  void shape(Xml &xml, const Shape &s, bool inline_shape) {
    const auto &o = *s.object;
    xml.open("PxShape");
    if (!inline_shape)
      xml.element("Id", std::to_string(serial(o.id)));
    geometry(xml, s.extracted->geometry);
    xml.element("LocalPose", transform(pose(o, "LocalPose")));
    for (const auto *p : {"SimulationFilterData", "QueryFilterData"}) {
      const auto &value = need(o, p);
      std::string out;
      for (std::size_t i = 0; i < 4; ++i) {
        std::uint32_t v = 0;
        for (std::size_t j = 0; j < 4; ++j)
          v |= std::uint32_t(value.raw_bytes()[i * 4 + j])
               << ((value.byte_order == ByteOrder::little ? j : 3 - j) * 8);
        if (i)
          out += ' ';
        out += std::to_string(v);
      }
      xml.element(p, out);
    }
    xml.open("Materials");
    for (auto id : s.materials)
      xml.element("PxMaterialRef", std::to_string(serial(id)));
    xml.close("Materials");
    xml.element("ContactOffset", number(scalar(o, "ContactOffset")));
    xml.element("RestOffset", number(scalar(o, "RestOffset")));
    xml.element("Flags", shape_flags(integer(o, "Flags")));
    xml.element("Name", text(o, "Name"));
    xml.close("PxShape");
  }
  Result output() {
    allocate_ids();
    Xml xml{{}, options.max_xml_bytes, options.cancelled};
    xml.add("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<PhysX30Collection "
            "version=\"3.4.2\">\n");
    xml.element("UpVector", "0 1 0");
    for (const auto &o : snapshot.instances)
      if (is(o.second, "PxPhysics") &&
          o.second.property("TolerancesScale.Length") &&
          o.second.property("TolerancesScale.Speed")) {
        xml.open("Scale");
        xml.element("Length",
                    number(scalar(o.second, "TolerancesScale.Length")));
        xml.element("Mass",
                    number(o.second.property("TolerancesScale.Mass")
                               ? scalar(o.second, "TolerancesScale.Mass")
                               : 1000.f));
        xml.element("Speed", number(scalar(o.second, "TolerancesScale.Speed")));
        xml.close("Scale");
        break;
      }
    if (!result.complete)
      xml.add("<!-- PARTIAL pvd_core export: unsupported requested content was "
              "omitted; inspect export diagnostics. -->\n");
    for (const auto &diagnostic : result.diagnostics) {
      auto comment = "Source " + std::to_string(diagnostic.object.id) + "/" +
                     std::to_string(diagnostic.object.generation) + " " +
                     diagnostic.property + ": " + diagnostic.message;
      for (std::size_t pos = 0;
           (pos = comment.find("--", pos)) != std::string::npos; pos += 3)
        comment.replace(pos, 2, "- -");
      xml.add("<!-- " + escape(comment) + " -->\n");
    }
    for (const auto &entry : materials) {
      const auto &o = *entry.second;
      xml.open("PxMaterial");
      xml.element("Id", std::to_string(serial(o.id)));
      for (const auto *p : {"DynamicFriction", "StaticFriction", "Restitution"})
        xml.element(p, number(scalar(o, p)));
      xml.element("Flags",
                  flags(integer(o, "Flags"),
                        {"eDISABLE_FRICTION", "eDISABLE_STRONG_FRICTION"}));
      xml.element("FrictionCombineMode",
                  combine(integer(o, "FrictionCombineMode")));
      xml.element("RestitutionCombineMode",
                  combine(integer(o, "RestitutionCombineMode")));
      xml.close("PxMaterial");
    }
    for (const auto &entry : resources) {
      const auto &r = entry.second;
      const auto &o = *r.object;
      const char *type =
          r.kind == render::GeometryKind::height_field  ? "PxHeightField"
          : r.kind == render::GeometryKind::convex_mesh ? "PxConvexMesh"
                                                        : "PxBVH33TriangleMesh";
      xml.open(type);
      xml.element("Id", std::to_string(serial(o.id)));
      if (r.kind == render::GeometryKind::height_field) {
        xml.element("NbRows", std::to_string(integer(o, "NbRows")));
        xml.element("NbColumns", std::to_string(integer(o, "NbColumns")));
        xml.element("Format", "eS16_TM");
        xml.element("Thickness", number(scalar(o, "Thickness")));
        xml.element("ConvexEdgeThreshold",
                    number(scalar(o, "ConvexEdgeThreshold")));
        xml.element("Flags",
                    flags(integer(o, "Flags"), {"eNO_BOUNDARY_EDGES"}));
        xml.open("samples");
        const auto &samples = need(o, "Samples");
        for (std::size_t i = 0; i < samples.item_count; ++i) {
          const auto &b = samples.raw_bytes();
          const auto h =
              samples.byte_order == ByteOrder::little
                  ? std::uint32_t(b[i * 4]) | (std::uint32_t(b[i * 4 + 1]) << 8)
                  : std::uint32_t(b[i * 4 + 1]) |
                        (std::uint32_t(b[i * 4]) << 8);
          const auto packed = h | (std::uint32_t(b[i * 4 + 2]) << 16) |
                              (std::uint32_t(b[i * 4 + 3]) << 24);
          xml.add(std::to_string(packed) + " ");
        }
        xml.close("samples");
      } else {
        const char *points =
            r.kind == render::GeometryKind::convex_mesh ? "points" : "Points";
        xml.open(points);
        for (const auto &vertex : r.mesh->vertices)
          xml.add(vec(vertex) + " ");
        xml.close(points);
        if (r.kind == render::GeometryKind::triangle_mesh) {
          xml.open("Triangles");
          for (auto index : r.mesh->indices)
            xml.add(std::to_string(index) + " ");
          xml.close("Triangles");
          if (!r.mesh->material_indices.empty()) {
            xml.open("materialIndices");
            for (auto index : r.mesh->material_indices)
              xml.add(std::to_string(index) + " ");
            xml.close("materialIndices");
          }
        }
      }
      xml.close(type);
    }
    for (const auto &entry : shapes)
      if (!entry.second.exclusive)
        shape(xml, entry.second, false);
    for (const auto &entry : actors) {
      const auto &a = entry.second;
      const auto &o = *a.object;
      const char *type = a.dynamic ? "PxRigidDynamic" : "PxRigidStatic";
      xml.open(type);
      xml.element("Id", std::to_string(serial(o.id)));
      xml.element("Name", text(o, "Name"));
      xml.element("ActorFlags", actor_flags(integer(o, "ActorFlags")));
      xml.element("DominanceGroup",
                  std::to_string(integer(o, "DominanceGroup")));
      xml.element("OwnerClient", std::to_string(integer(o, "OwnerClient")));
      if (o.property("ClientBehaviorFlags"))
        xml.element("ClientBehaviorFlags",
                    flags(integer(o, "ClientBehaviorFlags"),
                          {"eREPORT_TO_FOREIGN_CLIENTS_TRIGGER_NOTIFY",
                           "eREPORT_TO_FOREIGN_CLIENTS_CONTACT_NOTIFY",
                           "eREPORT_TO_FOREIGN_CLIENTS_CONSTRAINT_BREAK_NOTIFY",
                           "eREPORT_TO_FOREIGN_CLIENTS_SCENE_QUERY"}));
      xml.element("GlobalPose", transform(pose(o, "GlobalPose")));
      xml.open("Shapes");
      for (auto id : a.shapes) {
        const auto &s = shapes.at(id);
        if (s.exclusive)
          shape(xml, s, true);
        else
          xml.element("PxShapeRef", std::to_string(serial(id)));
      }
      xml.close("Shapes");
      if (a.dynamic) {
        xml.element("CMassLocalPose", transform(pose(o, "CMassLocalPose")));
        xml.element("Mass", number(scalar(o, "Mass")));
        xml.element("MassSpaceInertiaTensor",
                    vec(vector(o, "MassSpaceInertiaTensor")));
        xml.element("LinearVelocity", vec(vector(o, "LinearVelocity")));
        xml.element("AngularVelocity", vec(vector(o, "AngularVelocity")));
        xml.element("RigidBodyFlags", body_flags(integer(o, "RigidBodyFlags")));
        if (o.property("KinematicTarget"))
          xml.element("KinematicTarget", transform(pose(o, "KinematicTarget")));
        if (o.property("RigidDynamicLockFlags"))
          xml.element("RigidDynamicLockFlags",
                      lock_flags(integer(o, "RigidDynamicLockFlags")));
        for (const auto *p :
             {"MinCCDAdvanceCoefficient", "MaxDepenetrationVelocity",
              "MaxContactImpulse", "LinearDamping", "AngularDamping",
              "MaxAngularVelocity", "SleepThreshold", "StabilizationThreshold",
              "WakeCounter", "ContactReportThreshold"})
          xml.element(p, number(scalar(o, p)));
        xml.open("SolverIterationCounts");
        xml.element("minPositionIters",
                    std::to_string(
                        integer(o, "SolverIterationCounts.minPositionIters")));
        xml.element("minVelocityIters",
                    std::to_string(
                        integer(o, "SolverIterationCounts.minVelocityIters")));
        xml.close("SolverIterationCounts");
      }
      xml.close(type);
    }
    xml.close("PhysX30Collection");
    result.xml = std::move(xml.data);
    result.actor_count = actors.size();
    result.shape_count = shapes.size();
    result.material_count = materials.size();
    result.resource_count = resources.size();
    return std::move(result);
  }
};
} // namespace
Result export_snapshot(const Metadata &metadata, const Snapshot &snapshot,
                       const Options &options) {
  Build build(metadata, snapshot, options);
  build.check();
  build.select();
  render::ExtractorOptions limits;
  limits.max_vertices = options.max_vertices;
  limits.max_triangles = options.max_triangles;
  limits.max_heightfield_cells = options.max_heightfield_samples;
  render::Extractor extractor(limits);
  render::ExtractOptions extract;
  if (options.scene)
    extract.scene_id = options.scene->id;
  auto input = build.geometry_input();
  auto frame = extractor.extract(metadata, input, extract);
  for (const auto &diagnostic : frame.diagnostics)
    if (build.selected.count(diagnostic.actor_id)) {
      const auto *actor = snapshot.instance(diagnostic.actor_id);
      if (actor)
        build.issue(*actor, diagnostic.property, diagnostic.message);
    }
  build.prepare(frame);
  return build.output();
}
Result export_session(const Session &session, std::size_t event_count,
                      const Options &options) {
  if (event_count == std::numeric_limits<std::size_t>::max())
    event_count = session.event_count();
  return export_snapshot(session.metadata(),
                         session.snapshot_at_event(event_count), options);
}
void write(std::ostream &out, const Result &result) {
  if (result.xml.empty())
    throw ExportError("empty RepX result");
  out.write(result.xml.data(), static_cast<std::streamsize>(result.xml.size()));
  if (!out)
    throw ExportError("RepX output stream write failed");
}
void save_file(const std::string &utf8, const Result &result, bool overwrite,
               std::function<bool()> cancelled) {
  if (result.xml.empty())
    throw ExportError("empty RepX result");
  if (utf8.empty() || utf8.find('\0') != std::string::npos)
    throw ExportError("invalid RepX save path");
  auto check = [&] {
    if (cancelled && cancelled())
      throw ExportCancelled{};
  };
  check();
  const auto destination = std::filesystem::u8path(utf8);
  if (destination.filename().empty())
    throw ExportError("RepX destination needs a filename");
  if (!overwrite && std::filesystem::exists(destination))
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "RepX destination exists");
  struct File {
    int fd = -1;
    std::filesystem::path path;
    bool published = false, owned = false;
    ~File() {
      if (fd >= 0) {
#ifdef _WIN32
        _close(fd);
#else
        ::close(fd);
#endif
      }
      if (owned && !published) {
        std::error_code error;
        std::filesystem::remove(path, error);
      }
    }
  } file;
  static std::atomic<std::uint64_t> sequence{0};
  for (unsigned tries = 0; tries < 128; ++tries) {
    file.path = destination;
    file.path += std::filesystem::path(
        ".pvd-tmp-" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(++sequence));
#ifdef _WIN32
    const auto error = _wsopen_s(&file.fd, file.path.c_str(),
                                 _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                                 _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (error) {
      file.fd = -1;
      errno = error;
    }
#else
    file.fd = ::open(file.path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
    if (file.fd >= 0) {
      file.owned = true;
      break;
    }
    if (errno != EEXIST)
      throw std::system_error(errno, std::generic_category(),
                              "create private RepX file");
  }
  if (file.fd < 0)
    throw ExportError("cannot allocate private RepX filename");
  for (std::size_t at = 0; at < result.xml.size();) {
    check();
    const auto size = static_cast<unsigned>(
        std::min<std::size_t>(1024u * 1024u, result.xml.size() - at));
#ifdef _WIN32
    const auto n = _write(file.fd, result.xml.data() + at, size);
#else
    const auto n = ::write(file.fd, result.xml.data() + at, size);
#endif
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::system_error(errno ? errno : EIO, std::generic_category(),
                              "write RepX file");
    at += static_cast<std::size_t>(n);
  }
#ifdef _WIN32
  if (_commit(file.fd))
    throw std::system_error(errno, std::generic_category(), "flush RepX file");
  const auto fd = file.fd;
  file.fd = -1;
  if (_close(fd))
    throw std::system_error(errno, std::generic_category(), "close RepX file");
  check();
  if (!MoveFileExW(file.path.c_str(), destination.c_str(),
                   MOVEFILE_WRITE_THROUGH |
                       (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), "publish RepX file");
#else
  if (::fsync(file.fd))
    throw std::system_error(errno, std::generic_category(), "flush RepX file");
  const auto fd = file.fd;
  file.fd = -1;
  if (::close(fd))
    throw std::system_error(errno, std::generic_category(), "close RepX file");
  check();
  if (overwrite) {
    if (::rename(file.path.c_str(), destination.c_str()))
      throw std::system_error(errno, std::generic_category(),
                              "publish RepX file");
  } else {
    if (::link(file.path.c_str(), destination.c_str()))
      throw std::system_error(errno, std::generic_category(),
                              "publish exclusive RepX file");
    ::unlink(file.path.c_str());
  }
#endif
  file.published = true;
}
} // namespace pvd::repx
