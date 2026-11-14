#include "pvd/render.hpp"
#include "pvd/repx.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
namespace xml = pvd::repx;
void check(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
template <class F> void rejects(F &&f) {
  try {
    f();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("expected RepX rejection");
}
Bytes integer_bytes(std::uint64_t value, std::size_t width) {
  Bytes out;
  for (std::size_t i = 0; i < width; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
  return out;
}
PropertyValue raw(const char *type, Bytes bytes, std::uint64_t count = 1) {
  PropertyValue value;
  value.incoming_type = {"physx3", type};
  value.pointer_width = 8;
  value.item_count = count;
  value.bytes = std::make_shared<const Bytes>(std::move(bytes));
  return value;
}
PropertyValue ui(std::uint64_t value) {
  return raw("PvdU32", integer_bytes(value, 4));
}
PropertyValue floats(const char *type, std::initializer_list<float> values) {
  Bytes out;
  for (auto value : values) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    auto b = integer_bytes(bits, 4);
    out.insert(out.end(), b.begin(), b.end());
  }
  return raw(type, std::move(out));
}
PropertyValue string(const std::string &text) {
  Bytes data(8, 0);
  data.insert(data.end(), text.begin(), text.end());
  data.push_back(0);
  auto value = raw("String", std::move(data));
  value.text = text;
  return value;
}
PropertyValue refs(std::initializer_list<std::uint64_t> ids) {
  Bytes data;
  for (auto id : ids) {
    auto b = integer_bytes(id, 8);
    data.insert(data.end(), b.begin(), b.end());
  }
  auto value = raw("VoidPtr", std::move(data), ids.size());
  value.reference_ids = std::vector<std::uint64_t>(ids);
  return value;
}
struct Fixture {
  Metadata metadata;
  Snapshot state;
  Fixture() {
    for (const auto *name :
         {"PxActor", "PxRigidActor", "PxRigidStatic", "PxShape", "PxGeometry",
          "PxBoxGeometry", "PxScene", "PxMaterial", "PxCloth"}) {
      QualifiedName q{"physx3", name};
      metadata.classes.emplace(q, ClassDefinition{q, {}, {}});
    }
    metadata.classes.at({"physx3", "PxRigidActor"}).parent =
        QualifiedName{"physx3", "PxActor"};
    metadata.classes.at({"physx3", "PxRigidStatic"}).parent =
        QualifiedName{"physx3", "PxRigidActor"};
    metadata.classes.at({"physx3", "PxBoxGeometry"}).parent =
        QualifiedName{"physx3", "PxGeometry"};
    metadata.classes.at({"physx3", "PxCloth"}).parent =
        QualifiedName{"physx3", "PxActor"};
    auto add = [&](std::uint64_t id, const char *type) {
      InstanceState value;
      value.id = id;
      value.class_name = {"physx3", type};
      state.instances.emplace(id, std::move(value));
    };
    add(1, "PxScene");
    add(100, "PxRigidStatic");
    add(10, "PxShape");
    add(20, "PxBoxGeometry");
    add(30, "PxMaterial");
    auto &actor = state.instances.at(100).properties;
    actor["Name"] = string(u8"\t  Actor <&> \"quoted\" '中文'\r\nlast \t");
    actor["ActorFlags"] = ui(3);
    actor["DominanceGroup"] = ui(2);
    actor["OwnerClient"] = ui(0);
    actor["ClientBehaviorFlags"] = ui(0);
    actor["GlobalPose"] = floats("PxTransform", {0, 0, 0, 1, 1, 2, 3});
    actor["Scene"] = refs({1});
    actor["Shapes"] = refs({10});
    auto &shape = state.instances.at(10).properties;
    shape["Name"] = string("  shape & <tag>  ");
    shape["Geometry"] = refs({20});
    shape["Materials"] = refs({30});
    shape["Actor"] = refs({100});
    shape["Flags"] = ui(11);
    shape["IsExclusive"] = ui(1);
    shape["LocalPose"] = floats("PxTransform", {0, 0, 0, 1, .2f, .3f, .4f});
    shape["ContactOffset"] = floats("PvdF32", {.02f});
    shape["RestOffset"] = floats("PvdF32", {0});
    shape["SimulationFilterData"] = raw("PxFilterData", Bytes(16, 0));
    shape["QueryFilterData"] = raw("PxFilterData", Bytes(16, 0));
    state.instances.at(20).properties["HalfExtents"] =
        floats("PxVec3", {.5f, 1, 2});
    auto &material = state.instances.at(30).properties;
    material["StaticFriction"] = floats("PvdF32", {.4f});
    material["DynamicFriction"] = floats("PvdF32", {.5f});
    material["Restitution"] = floats("PvdF32", {.2f});
    material["Flags"] = ui(2);
    material["FrictionCombineMode"] = ui(3);
    material["RestitutionCombineMode"] = ui(0);
  }
};
std::string number(float value) {
  char b[64];
  auto r = std::to_chars(b, b + sizeof(b), value, std::chars_format::general,
                         std::numeric_limits<float>::max_digits10);
  check(r.ec == std::errc{}, "CSV float formatting");
  return std::string(b, r.ptr);
}
std::string hex(const std::string &text) {
  const char *digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : text) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}
struct Csv {
  std::ostream &out;
  xml::ObjectMapping mapping;
  void prefix(char type, const std::string &name, std::size_t i) {
    out << type << ',' << mapping.serial_id << ',' << mapping.owner_serial_id
        << ',' << mapping.shape_index << ',' << name << ',' << i << ',';
  }
  void u(const std::string &name, std::uint64_t value, std::size_t i = 0) {
    prefix('u', name, i);
    out << value << '\n';
  }
  void f(const std::string &name, float value, std::size_t i = 0) {
    prefix('f', name, i);
    out << number(value) << '\n';
  }
  void s(const std::string &name, const std::string &value) {
    prefix('s', name, 0);
    out << hex(value) << '\n';
  }
  void vec(const std::string &name, Vec3 v, std::size_t i = 0) {
    f(name, v.x, i);
    f(name, v.y, i + 1);
    f(name, v.z, i + 2);
  }
  void quat(const std::string &name, Quat q, std::size_t i = 0) {
    f(name, q.x, i);
    f(name, q.y, i + 1);
    f(name, q.z, i + 2);
    f(name, q.w, i + 3);
  }
  void pose(const std::string &name, Transform t, std::size_t i = 0) {
    quat(name, t.rotation, i);
    vec(name, t.position, i + 4);
  }
};
const PropertyValue &need(const InstanceState &o, const char *property) {
  const auto *p = o.property(property);
  check(p != nullptr, "expected fixture property exists");
  return *p;
}
std::uint64_t uint(const InstanceState &o, const char *p) {
  auto v = need(o, p).as_uint();
  check(bool(v), "expected integer property");
  return *v;
}
float scalar(const InstanceState &o, const char *p) {
  auto v = need(o, p).as_float();
  check(bool(v), "expected scalar property");
  return *v;
}
Vec3 vec(const InstanceState &o, const char *p) {
  auto v = need(o, p).as_vec3();
  check(bool(v), "expected vector property");
  return *v;
}
Transform pose(const InstanceState &o, const char *p) {
  auto v = need(o, p).as_transform();
  check(bool(v), "expected pose property");
  return *v;
}
bool less(Vec3 a, Vec3 b) {
  return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
}
void resource_triangles(Csv &csv, const render::MeshData &mesh) {
  std::vector<std::array<Vec3, 3>> all;
  for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
    std::array<Vec3, 3> t{mesh.vertices[mesh.indices[i]],
                          mesh.vertices[mesh.indices[i + 1]],
                          mesh.vertices[mesh.indices[i + 2]]};
    const auto minimum = static_cast<std::size_t>(
        std::min_element(t.begin(), t.end(), less) - t.begin());
    std::rotate(t.begin(), t.begin() + minimum, t.end());
    all.push_back(t);
  }
  std::sort(all.begin(), all.end(), [](const auto &a, const auto &b) {
    for (std::size_t i = 0; i < 3; ++i) {
      if (less(a[i], b[i]))
        return true;
      if (less(b[i], a[i]))
        return false;
    }
    return false;
  });
  for (std::size_t i = 0; i < all.size(); ++i) {
    for (std::size_t j = 0; j < 3; ++j)
      csv.vec("TriangleVertex", all[i][j], i * 9 + j * 3);
    const Vec3 a{all[i][1].x - all[i][0].x, all[i][1].y - all[i][0].y,
                 all[i][1].z - all[i][0].z},
        b{all[i][2].x - all[i][0].x, all[i][2].y - all[i][0].y,
          all[i][2].z - all[i][0].z};
    Vec3 normal{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
    const auto length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                  normal.z * normal.z);
    if (length > 0) {
      normal.x /= length;
      normal.y /= length;
      normal.z /= length;
    }
    csv.vec("TriangleNormal", normal, i * 3);
  }
}
struct JointOracleRow {
  std::string name, type, property, value;
  std::size_t index = 0;
  char kind = 'f';
};
using JointOracle = std::map<std::uint64_t, std::vector<JointOracleRow>>;
float joint_float(const InstanceState &object, const JointOracleRow &row) {
  const auto &value = need(object, row.property.c_str());
  if (value.incoming_type.name == "PxTransform") {
    const auto v = value.as_transform();
    check(bool(v) && row.index < 7, "joint oracle transform field");
    const float parts[] = {v->rotation.x, v->rotation.y, v->rotation.z,
                           v->rotation.w, v->position.x, v->position.y,
                           v->position.z};
    return parts[row.index];
  }
  if (value.incoming_type.name == "PxVec3") {
    const auto v = value.as_vec3();
    check(bool(v) && row.index < 3, "joint oracle Vec3 field");
    const float parts[] = {v->x, v->y, v->z};
    return parts[row.index];
  }
  if (value.incoming_type.name == "PxQuat") {
    const auto v = value.as_quat();
    check(bool(v) && row.index < 4, "joint quaternion component");
    const float parts[] = {v->x, v->y, v->z, v->w};
    return parts[row.index];
  }
  check(row.index == 0, "joint scalar oracle index");
  return scalar(object, row.property.c_str());
}
std::uint64_t joint_uint(const InstanceState &object,
                         const JointOracleRow &row) {
  if (row.property.rfind("Actors.", 0) == 0) {
    const auto v = need(object, row.property.c_str()).as_object_ref();
    check(bool(v), "joint actor reference");
    return *v;
  }
  return uint(object, row.property.c_str());
}
std::uint64_t articulation_owner(const Snapshot &s, const InstanceState &link) {
  const auto *current = &link;
  std::set<std::uint64_t> seen;
  while (current->class_name.name == "PxArticulationLink") {
    check(seen.insert(current->id).second && seen.size() <= 64,
          "oracle parent cycle/limit");
    const auto id = need(*current, "Parent").as_object_ref();
    check(bool(id), "oracle parent encoding");
    current = s.instance(*id);
    check(current, "oracle parent exists");
  }
  check(current->class_name.name == "PxArticulation",
        "oracle parent terminates at articulation");
  return current->id;
}
std::vector<std::uint64_t> unique_refs(const InstanceState &o, const char *p) {
  std::vector<std::uint64_t> refs;
  if (const auto *v = o.property(p))
    for (std::size_t i = 0; i < v->item_count; ++i) {
      const auto id = v->as_object_ref(i);
      check(bool(id), "oracle reference value");
      if (std::find(refs.begin(), refs.end(), *id) == refs.end())
        refs.push_back(*id);
    }
  return refs;
}
void expected(std::ostream &out, const Metadata &metadata,
              const Snapshot &state, const xml::Result &result,
              const xml::Options &options,
              const JointOracle *joint_oracle = nullptr) {
  render::Extractor extractor;
  auto frame = extractor.extract(metadata, state);
  std::map<std::uint64_t, const render::ShapeInstance *> shapes;
  std::map<std::uint64_t, std::shared_ptr<const render::MeshData>> meshes;
  std::map<std::uint64_t, std::uint64_t> ids;
  for (const auto &mapping : result.objects)
    ids[mapping.source.id] = mapping.serial_id;
  for (const auto &shape : frame.instances) {
    shapes.emplace(shape.shape_id, &shape);
    std::visit(
        [&](const auto &g) {
          using T = std::decay_t<decltype(g)>;
          if constexpr (std::is_same_v<T, render::TriangleMesh> ||
                        std::is_same_v<T, render::ConvexMesh> ||
                        std::is_same_v<T, render::HeightField>)
            meshes.emplace(g.mesh->resource.id, g.mesh);
        },
        shape.geometry);
  }
  out << "kind,serial_id,owner_id,shape_index,field,index,value\n";
  for (const auto &mapping : result.objects) {
    const auto *object = state.instance(mapping.source.id);
    check(object && object->generation == mapping.source.generation,
          "mapping source identity");
    const auto &o = *object;
    out << "object," << mapping.serial_id << ',' << mapping.owner_serial_id
        << ',' << mapping.shape_index << ',' << mapping.type << '\n';
    Csv c{out, mapping};
    if (mapping.type == "PxRigidStatic" || mapping.type == "PxRigidDynamic" ||
        mapping.type == "PxArticulationLink") {
      c.s("Name", o.property("Name") ? need(o, "Name").text.value_or("") : "");
      c.pose("GlobalPose", pose(o, "GlobalPose"));
      for (const auto *p : {"ActorFlags", "DominanceGroup", "OwnerClient"})
        c.u(p, uint(o, p));
      if (o.property("ClientBehaviorFlags"))
        c.u("ClientBehaviorFlags", uint(o, "ClientBehaviorFlags"));
      auto refs = mapping.type == "PxArticulationLink"
                      ? unique_refs(o, "Shapes")
                      : *need(o, "Shapes").reference_ids;
      if (options.selection == xml::Selection::attachments)
        refs.erase(
            std::remove_if(refs.begin(), refs.end(),
                           [&](auto id) {
                             return std::none_of(
                                 options.attachments.begin(),
                                 options.attachments.end(), [&](const auto &a) {
                                   return a.actor.id == o.id &&
                                          a.actor.generation == o.generation &&
                                          a.shape.id == id;
                                 });
                           }),
            refs.end());
      c.u("ShapeCount", refs.size());
      for (std::size_t i = 0; i < refs.size(); ++i) {
        c.u("ShapeRef", ids.at(refs[i]), i);
        c.pose("ShapeWorldPose",
               render::compose(pose(o, "GlobalPose"),
                               pose(*state.instance(refs[i]), "LocalPose")),
               i * 7);
      }
      if (mapping.type == "PxRigidDynamic" ||
          mapping.type == "PxArticulationLink") {
        c.pose("CMassLocalPose", pose(o, "CMassLocalPose"));
        c.f("Mass", scalar(o, "Mass"));
        c.vec("MassSpaceInertiaTensor", vec(o, "MassSpaceInertiaTensor"));
        const bool kinematic = (uint(o, "RigidBodyFlags") & 1) != 0;
        c.vec("LinearVelocity", kinematic ? Vec3{} : vec(o, "LinearVelocity"));
        c.vec("AngularVelocity",
              kinematic ? Vec3{} : vec(o, "AngularVelocity"));
        c.u("RigidBodyFlags", uint(o, "RigidBodyFlags"));
        if (mapping.type == "PxArticulationLink") {
          for (const auto *p :
               {"MinCCDAdvanceCoefficient", "MaxDepenetrationVelocity",
                "MaxContactImpulse"})
            c.f(p, scalar(o, p));
          const auto owner = articulation_owner(state, o);
          c.u("ArticulationRef", ids.at(owner));
          const auto parent = need(o, "Parent").as_object_ref().value();
          c.u("ParentRef", parent == owner ? 0 : ids.at(parent));
          auto children = unique_refs(o, "Links");
          for (auto &id : children)
            id = ids.at(id);
          std::sort(children.begin(), children.end());
          c.u("ChildCount", children.size());
          for (std::size_t i = 0; i < children.size(); ++i)
            c.u("ChildRef", children[i], i);
          continue;
        }
        c.u("HasKinematicTarget", 0);
        if (o.property("RigidDynamicLockFlags"))
          c.u("RigidDynamicLockFlags", uint(o, "RigidDynamicLockFlags"));
        for (const auto *p :
             {"LinearDamping", "AngularDamping", "MaxAngularVelocity",
              "MinCCDAdvanceCoefficient", "MaxDepenetrationVelocity",
              "MaxContactImpulse", "SleepThreshold", "StabilizationThreshold",
              "WakeCounter", "ContactReportThreshold"})
          c.f(p, kinematic && std::string(p) == "WakeCounter" ? 0.f
                                                              : scalar(o, p));
        c.u("minPositionIters",
            uint(o, "SolverIterationCounts.minPositionIters"));
        c.u("minVelocityIters",
            uint(o, "SolverIterationCounts.minVelocityIters"));
      }
    } else if (mapping.type == "PxArticulation" ||
               mapping.type == "PxArticulationJoint") {
      check(joint_oracle && joint_oracle->count(o.id),
            "independent native-articulation property list required");
      if (mapping.type == "PxArticulation") {
        c.s("Name", need(o, "Name").text.value_or(""));
        std::vector<std::uint64_t> links;
        for (const auto &entry : result.objects)
          if (entry.type == "PxArticulationLink" &&
              articulation_owner(state, *state.instance(entry.source.id)) ==
                  o.id)
            links.push_back(entry.serial_id);
        std::sort(links.begin(), links.end());
        c.u("LinkCount", links.size());
        for (std::size_t i = 0; i < links.size(); ++i)
          c.u("LinkRef", links[i], i);
      }
      for (const auto &row : joint_oracle->at(o.id))
        if (row.kind == 'f')
          c.f(row.property, joint_float(o, row), row.index);
        else if (row.kind == 'u')
          c.u(row.property, uint(o, row.property.c_str()), row.index);
    } else if (mapping.type == "PxAggregate") {
      c.u("MaxNbActors", uint(o, "MaxNbActors"));
      c.u("SelfCollision", uint(o, "SelfCollision"));
      auto members = unique_refs(o, "Actors");
      const auto arts = unique_refs(o, "Articulations");
      for (const auto &entry : result.objects)
        if (entry.type == "PxArticulationLink") {
          const auto owner =
              articulation_owner(state, *state.instance(entry.source.id));
          if (std::find(arts.begin(), arts.end(), owner) != arts.end())
            members.push_back(entry.source.id);
        }
      for (auto &id : members)
        id = ids.at(id);
      std::sort(members.begin(), members.end());
      c.u("ActorCount", members.size());
      for (std::size_t i = 0; i < members.size(); ++i)
        c.u("ActorRef", members[i], i);
    } else if (mapping.type.find("Joint") != std::string::npos) {
      check(joint_oracle && joint_oracle->count(o.id),
            "independent joint getter oracle required");
      c.s("Name", o.property("Name") ? need(o, "Name").text.value_or("") : "");
      for (const auto &row : joint_oracle->at(o.id))
        if (row.kind == 'f')
          c.f(row.property, joint_float(o, row), row.index);
        else {
          const auto value = joint_uint(o, row);
          c.u(row.property,
              row.property.rfind("Actors.", 0) == 0
                  ? (value ? ids.at(value) : 0)
                  : value,
              row.index);
        }
    } else if (mapping.type == "PxMaterial") {
      for (const auto *p : {"StaticFriction", "DynamicFriction", "Restitution"})
        c.f(p, scalar(o, p));
      for (const auto *p :
           {"Flags", "FrictionCombineMode", "RestitutionCombineMode"})
        c.u(p, uint(o, p));
    } else if (mapping.type == "PxShape") {
      c.s("Name", o.property("Name") ? need(o, "Name").text.value_or("") : "");
      c.pose("LocalPose", pose(o, "LocalPose"));
      c.u("IsExclusive", uint(o, "IsExclusive"));
      c.u("Flags", uint(o, "Flags"));
      c.f("ContactOffset", scalar(o, "ContactOffset"));
      c.f("RestOffset", scalar(o, "RestOffset"));
      for (const auto *p : {"SimulationFilterData", "QueryFilterData"}) {
        const auto &v = need(o, p);
        for (std::size_t i = 0; i < 4; ++i) {
          std::uint32_t u = 0;
          for (std::size_t j = 0; j < 4; ++j)
            u |= std::uint32_t(v.raw_bytes()[i * 4 + j])
                 << ((v.byte_order == ByteOrder::little ? j : 3 - j) * 8);
          c.u(p, u, i);
        }
      }
      const auto &refs = *need(o, "Materials").reference_ids;
      c.u("MaterialCount", refs.size());
      for (std::size_t i = 0; i < refs.size(); ++i)
        c.u("MaterialRef", ids.at(refs[i]), i);
      const auto &shape = *shapes.at(o.id);
      static const unsigned sdk_types[] = {3, 0, 2, 1, 4, 5, 6};
      c.u("GeometryType", sdk_types[shape.geometry.index()]);
      std::visit(
          [&](const auto &g) {
            using T = std::decay_t<decltype(g)>;
            if constexpr (std::is_same_v<T, render::Box>)
              c.vec("HalfExtents", g.half_extents);
            else if constexpr (std::is_same_v<T, render::Sphere>)
              c.f("Radius", g.radius);
            else if constexpr (std::is_same_v<T, render::Capsule>) {
              c.f("Radius", g.radius);
              c.f("HalfHeight", g.half_height);
            } else if constexpr (std::is_same_v<T, render::TriangleMesh> ||
                                 std::is_same_v<T, render::ConvexMesh>) {
              c.vec("MeshScale", g.scale.scale);
              c.quat("MeshRotation", g.scale.rotation);
              c.u("MeshFlags", g.mesh_flags);
              c.u("ResourceRef", ids.at(g.mesh->resource.id));
            } else if constexpr (std::is_same_v<T, render::HeightField>) {
              c.f("HeightScale", g.height_scale);
              c.f("RowScale", g.row_scale);
              c.f("ColumnScale", g.column_scale);
              c.u("MeshFlags", g.flags);
              c.u("ResourceRef", ids.at(g.mesh->resource.id));
            }
          },
          shape.geometry);
    } else if (mapping.type == "PxHeightField") {
      for (const auto *p : {"NbRows", "NbColumns", "Format", "Flags"})
        c.u(p, uint(o, p));
      for (const auto *p : {"Thickness", "ConvexEdgeThreshold"})
        c.f(p, scalar(o, p));
      const auto &v = need(o, "Samples");
      for (std::size_t i = 0; i < v.item_count; ++i) {
        const auto &b = v.raw_bytes();
        const auto h =
            v.byte_order == ByteOrder::little
                ? std::uint32_t(b[i * 4]) | (std::uint32_t(b[i * 4 + 1]) << 8)
                : std::uint32_t(b[i * 4 + 1]) | (std::uint32_t(b[i * 4]) << 8);
        c.u("Sample",
            h | (std::uint32_t(b[i * 4 + 2]) << 16) |
                (std::uint32_t(b[i * 4 + 3]) << 24),
            i);
      }
    } else {
      auto mesh = meshes.at(o.id);
      auto vertices = mesh->vertices;
      std::sort(vertices.begin(), vertices.end(), less);
      c.u("VertexCount", vertices.size());
      for (std::size_t i = 0; i < vertices.size(); ++i)
        c.vec("Vertex", vertices[i], i * 3);
      if (mapping.type == "PxBVH33TriangleMesh") {
        c.u("TriangleCount", mesh->indices.size() / 3);
        resource_triangles(c, *mesh);
      }
    }
  }
  check(bool(out), "expected CSV written");
}
void emit_fixture(const std::string &prefix, const Metadata &metadata,
                  const Snapshot &snapshot, xml::Options options,
                  const JointOracle *joints = nullptr) {
  auto result = xml::export_snapshot(metadata, snapshot, options);
  xml::save_file(prefix + ".repx", result, true);
  std::ofstream csv(std::filesystem::u8path(prefix + "_expected.csv"));
  expected(csv, metadata, snapshot, result, options, joints);
  std::cout << "RepX export: " << result.actor_count << " actors, "
            << result.shape_count << " shapes, " << result.resource_count
            << " resources, " << result.joint_count
            << " joints, complete=" << result.complete << '\n';
}
Snapshot big_endian(Snapshot state) {
  for (auto &object : state.instances)
    for (auto &property : object.second.properties) {
      auto &value = property.second;
      if (value.byte_order == ByteOrder::big)
        continue;
      auto data = value.raw_bytes();
      const auto &name = value.incoming_type.name;
      std::size_t stride = 0;
      if (name == "PvdU16" || name == "PvdI16" || name == "PvdHullPolygonData")
        stride = 2;
      else if (name == "PvdU32" || name == "PvdI32" || name == "PvdF32" ||
               name == "StringHandle" || name == "PxVec2" || name == "PxVec3" ||
               name == "PxVec4" || name == "PxQuat" || name == "PxTransform" ||
               name == "PxBounds3" || name == "PxMat33" || name == "PxMat44" ||
               name == "PxFilterData" || name == "U32Array4")
        stride = 4;
      else if (name == "PvdU64" || name == "PvdI64" || name == "PvdF64")
        stride = 8;
      else if (name == "VoidPtr")
        stride = value.pointer_width;
      if (name == "String") {
        if (data.size() >= value.pointer_width)
          std::reverse(data.begin(), data.begin() + value.pointer_width);
      } else if (name == "PxHeightFieldSample") {
        for (std::size_t i = 0; i + 4 <= data.size(); i += 4)
          std::swap(data[i], data[i + 1]);
      } else if (name == "PxHullPolygon") {
        for (std::size_t i = 0; i + 20 <= data.size(); i += 20) {
          for (std::size_t j = 0; j < 16; j += 4)
            std::reverse(data.begin() + static_cast<std::ptrdiff_t>(i + j),
                         data.begin() + static_cast<std::ptrdiff_t>(i + j + 4));
          std::swap(data[i + 16], data[i + 17]);
          std::swap(data[i + 18], data[i + 19]);
        }
      } else if (stride) {
        check(data.size() % stride == 0, "BE typed data stride");
        for (std::size_t i = 0; i < data.size(); i += stride)
          std::reverse(data.begin() + static_cast<std::ptrdiff_t>(i),
                       data.begin() + static_cast<std::ptrdiff_t>(i + stride));
      }
      value.bytes = std::make_shared<const Bytes>(std::move(data));
      value.byte_order = ByteOrder::big;
    }
  return state;
}
void unit() {
  Fixture f;
  auto r = xml::export_snapshot(f.metadata, f.state);
  check(r.complete && r.actor_count == 1 && r.shape_count == 1 &&
            r.material_count == 1 && r.resource_count == 0,
        "basic rigid export counts");
  check(r.xml.find("&lt;&amp;&gt;") != std::string::npos &&
            r.xml.find("&quot;quoted&quot;") != std::string::npos &&
            r.xml.find("&apos;") != std::string::npos,
        "XML text escaped");
  check(r.objects.back().serial_id == 0 &&
            r.objects.back().owner_serial_id != 0,
        "exclusive shape has inline locator");
  check(xml::export_snapshot(f.metadata, big_endian(f.state)).xml == r.xml,
        "LE/BE equivalent snapshot produces identical XML");
  xml::Options attachment;
  attachment.selection = xml::Selection::attachments;
  attachment.attachments = {{{1, 0}, {100, 0}, {10, 0}}};
  check(xml::export_snapshot(f.metadata, f.state, attachment).xml == r.xml,
        "one exact attachment matches the complete single-shape actor");
  auto stale = attachment;
  stale.attachments[0].scene.generation = 1;
  rejects([&] { xml::export_snapshot(f.metadata, f.state, stale); });
  stale = attachment;
  stale.attachments[0].actor.generation = 1;
  rejects([&] { xml::export_snapshot(f.metadata, f.state, stale); });
  stale = attachment;
  stale.attachments[0].shape.generation = 1;
  rejects([&] { xml::export_snapshot(f.metadata, f.state, stale); });
  stale = attachment;
  stale.attachments[0].shape.id = 20;
  rejects([&] { xml::export_snapshot(f.metadata, f.state, stale); });
  auto multiple = f.state;
  auto hidden = multiple.instances.at(10);
  hidden.id = 11;
  hidden.properties["Name"] = string("hidden sibling");
  hidden.properties["Geometry"] = refs({21});
  multiple.instances.emplace(11, std::move(hidden));
  auto hidden_geometry = multiple.instances.at(20);
  hidden_geometry.id = 21;
  hidden_geometry.properties["HalfExtents"] = floats("PxVec3", {-1, 1, 1});
  multiple.instances.emplace(21, std::move(hidden_geometry));
  multiple.instances.at(100).properties["Shapes"] = refs({11, 10});
  rejects([&] { xml::export_snapshot(f.metadata, multiple); });
  auto subset = xml::export_snapshot(f.metadata, multiple, attachment);
  check(subset.complete && subset.actor_count == 1 && subset.shape_count == 1 &&
            subset.xml.find("hidden sibling") == std::string::npos,
        "hidden invalid sibling is excluded before extraction and validation");
  check(subset.objects.back().source.id == 10 &&
            subset.objects.back().shape_index == 0,
        "exclusive selected index rebased after hidden first shape");
  check(*multiple.instances.at(100).properties.at("Shapes").reference_ids ==
            std::vector<std::uint64_t>({11, 10}),
        "attachment filtering leaves caller snapshot immutable");
  multiple.instances.at(21).properties["HalfExtents"] =
      floats("PxVec3", {2, 1, 1});
  auto both = attachment;
  both.attachments.push_back({{1, 0}, {100, 0}, {11, 0}});
  both.attachments.push_back(both.attachments[0]);
  const auto all = xml::export_snapshot(f.metadata, multiple, both);
  check(all.shape_count == 2,
        "duplicate input attachment keys are deduplicated");
  check(all.objects[all.objects.size() - 2].source.id == 11 &&
            all.objects.back().source.id == 10,
        "selected attachment output preserves original actor shape order");
  xml::Options options;
  options.selection = xml::Selection::objects;
  options.objects = {{10, 0}};
  check(xml::export_snapshot(f.metadata, f.state, options).actor_count == 1,
        "shape selection expands owning actor");
  options.objects = {{100, 1}};
  rejects([&] { xml::export_snapshot(f.metadata, f.state, options); });
  options = {};
  options.scene = xml::ObjectKey{1, 0};
  check(xml::export_snapshot(f.metadata, f.state, options).actor_count == 1,
        "scene selection");
  options.scene = xml::ObjectKey{1, 1};
  rejects([&] { xml::export_snapshot(f.metadata, f.state, options); });
  options = {};
  options.max_xml_bytes = 10;
  rejects([&] { xml::export_snapshot(f.metadata, f.state, options); });
  options = {};
  options.max_objects = 1;
  rejects([&] { xml::export_snapshot(f.metadata, f.state, options); });
  options = {};
  options.cancelled = [] { return true; };
  rejects([&] { xml::export_snapshot(f.metadata, f.state, options); });
  auto malformed = f.state;
  malformed.instances.at(10).properties["Flags"] = ui(1u << 30);
  rejects([&] { xml::export_snapshot(f.metadata, malformed); });
  malformed = f.state;
  malformed.instances.at(100).properties["Name"] =
      string(std::string("bad\1name", 8));
  rejects([&] { xml::export_snapshot(f.metadata, malformed); });
  malformed = f.state;
  malformed.instances.at(100).properties["Name"] =
      string(std::string("\xc0\x80", 2));
  rejects([&] { xml::export_snapshot(f.metadata, malformed); });
  auto cloth = f.state;
  InstanceState unsupported;
  unsupported.id = 101;
  unsupported.class_name = {"physx3", "PxCloth"};
  unsupported.properties["Scene"] = refs({1});
  cloth.instances.emplace(101, std::move(unsupported));
  rejects([&] { xml::export_snapshot(f.metadata, cloth); });
  options = {};
  options.unsupported = xml::UnsupportedPolicy::allow_partial;
  auto partial = xml::export_snapshot(f.metadata, cloth, options);
  check(!partial.complete && !partial.diagnostics.empty() &&
            partial.xml.find("PARTIAL") != std::string::npos,
        "unsupported content is explicit partial");
  const auto dir =
      std::filesystem::temp_directory_path() /
      std::filesystem::u8path(
          "pvd-repx-tests-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()));
  check(std::filesystem::create_directory(dir), "new test directory");
  struct Cleanup {
    std::filesystem::path p;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(p, ec);
    }
  } cleanup{dir};
  const auto path = dir / std::filesystem::u8path(u8"导出 场景.repx");
  xml::save_file(path.u8string(), r);
  check(std::filesystem::file_size(path) == r.xml.size(),
        "UTF8 atomic file save");
  rejects([&] { xml::save_file(path.u8string(), partial); });
  rejects([&] {
    xml::save_file(path.u8string(), partial, true, [] { return true; });
  });
  check(std::filesystem::file_size(path) == r.xml.size(),
        "cancel overwrite retains original");
  xml::save_file(path.u8string(), partial, true);
  check(std::filesystem::file_size(path) == partial.xml.size(),
        "explicit overwrite");
  const auto canceled = dir / "cancel.repx";
  std::size_t checks = 0;
  rejects([&] {
    xml::save_file(canceled.u8string(), r, false, [&] { return ++checks > 1; });
  });
  check(!std::filesystem::exists(canceled), "cancelled save no destination");
  for (const auto &entry : std::filesystem::directory_iterator(dir))
    check(entry.path().filename().u8string().find(".pvd-tmp-") ==
              std::string::npos,
          "private files cleaned");
  std::ostringstream bad;
  bad.setstate(std::ios::badbit);
  rejects([&] { xml::write(bad, r); });
}
void real(const char *capture, const char *prefix) {
  auto source = Session::load_file(capture);
  check(!source.frames().empty(), "real capture has native frame");
  auto snapshot = source.snapshot_at_frame(0);
  xml::Options options;
  options.unsupported = xml::UnsupportedPolicy::allow_partial;
  emit_fixture(prefix, source.metadata(), snapshot, options);
  rejects([&] { xml::export_snapshot(source.metadata(), snapshot); });
  auto be = big_endian(snapshot);
  check(xml::export_snapshot(source.metadata(), be, options).xml ==
            xml::export_snapshot(source.metadata(), snapshot, options).xml,
        "real LE/BE geometry and flags agree");
  emit_fixture(std::string(prefix) + "_big_endian", source.metadata(), be,
               options);
  xml::Options selected;
  selected.selection = xml::Selection::objects;
  for (const auto &object : snapshot.instances)
    if (object.second.class_name == QualifiedName{"physx3", "PxRigidStatic"} &&
        object.second.property("Name") &&
        object.second.property("Name")->text == "triangle_reuse") {
      selected.objects = {{object.first, object.second.generation}};
      break;
    }
  check(!selected.objects.empty(),
        "known nonuniform mesh actor in SDK fixture");
  emit_fixture(std::string(prefix) + "_selected", source.metadata(), snapshot,
               selected);
  auto limited = selected;
  limited.max_vertices = 1;
  rejects([&] { xml::export_snapshot(source.metadata(), snapshot, limited); });
  for (const auto &object : snapshot.instances)
    if (object.second.class_name == QualifiedName{"physx3", "PxRigidStatic"} &&
        object.second.property("Name") &&
        object.second.property("Name")->text == "shared_actor_a") {
      const auto id = need(object.second, "Shapes").reference_ids->at(0);
      selected.objects = {{id, snapshot.instance(id)->generation}};
      break;
    }
  check(
      xml::export_snapshot(source.metadata(), snapshot, selected).actor_count ==
          2,
      "shared shape selection includes both owning actors");
  emit_fixture(std::string(prefix) + "_shared", source.metadata(), snapshot,
               selected);
  xml::Options attachment;
  attachment.selection = xml::Selection::attachments;
  for (const auto &object : snapshot.instances)
    if (object.second.class_name == QualifiedName{"physx3", "PxRigidStatic"} &&
        object.second.property("Name") &&
        object.second.property("Name")->text == "shared_actor_a") {
      const auto shape_id = need(object.second, "Shapes").reference_ids->at(0);
      const auto scene_id = *need(object.second, "Scene").as_object_ref();
      attachment.attachments = {
          {{scene_id, snapshot.instance(scene_id)->generation},
           {object.first, object.second.generation},
           {shape_id, snapshot.instance(shape_id)->generation}}};
      break;
    }
  const auto single_attachment =
      xml::export_snapshot(source.metadata(), snapshot, attachment);
  check(single_attachment.actor_count == 1 &&
            single_attachment.shape_count == 1 &&
            single_attachment.xml.find("shared_actor_b") == std::string::npos,
        "shared actor_a attachment excludes hidden actor_b owner");
  emit_fixture(std::string(prefix) + "_attachment", source.metadata(), snapshot,
               attachment);
  Fixture basic;
  emit_fixture(std::string(prefix) + "_synthetic", basic.metadata, basic.state,
               {});
}
std::vector<std::string> joint_csv_split(std::string line) {
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  std::vector<std::string> result;
  std::size_t at = 0;
  for (;;) {
    auto next = line.find(',', at);
    result.push_back(line.substr(at, next - at));
    if (next == std::string::npos)
      break;
    at = next + 1;
  }
  return result;
}
void native_articulation_unit() {
  Fixture f;
  for (const auto *name :
       {"PxRigidBody", "PxArticulationLink", "PxArticulation",
        "PxArticulationJoint", "PxAggregate"}) {
    QualifiedName q{"physx3", name};
    f.metadata.classes.emplace(q, ClassDefinition{q, {}, {}});
  }
  f.metadata.classes.at({"physx3", "PxRigidBody"}).parent =
      QualifiedName{"physx3", "PxRigidActor"};
  f.metadata.classes.at({"physx3", "PxArticulationLink"}).parent =
      QualifiedName{"physx3", "PxRigidBody"};
  auto &root = f.state.instances.at(100);
  root.class_name = {"physx3", "PxArticulationLink"};
  root.properties["Scene"] = refs({0});
  root.properties["Parent"] = refs({500});
  root.properties["Links"] = refs({101});
  root.properties["CMassLocalPose"] =
      floats("PxTransform", {0, 0, 0, 1, .1f, 0, 0});
  root.properties["Mass"] = floats("PvdF32", {1.25f});
  root.properties["MassSpaceInertiaTensor"] = floats("PxVec3", {1, 2, 3});
  root.properties["LinearVelocity"] = floats("PxVec3", {.1f, .2f, .3f});
  root.properties["AngularVelocity"] = floats("PxVec3", {.4f, .5f, .6f});
  root.properties["RigidBodyFlags"] = ui(0);
  root.properties["MinCCDAdvanceCoefficient"] = floats("PvdF32", {.2f});
  root.properties["MaxDepenetrationVelocity"] = floats("PvdF32", {100});
  root.properties["MaxContactImpulse"] = floats("PvdF32", {1000});
  auto child = root;
  child.id = 101;
  child.properties.erase("Links");
  child.properties["Shapes"] = refs({11, 11});
  child.properties["Parent"] = refs({100});
  child.properties["InboundJoint"] = refs({700});
  f.state.instances.emplace(101, std::move(child));
  auto shape = f.state.instances.at(10);
  shape.id = 11;
  shape.properties["Actor"] = refs({101});
  f.state.instances.emplace(11, std::move(shape));
  InstanceState art;
  art.id = 500;
  art.class_name = {"physx3", "PxArticulation"};
  art.properties["Scene"] = refs({1});
  art.properties["Links"] = refs({100});
  art.properties["IsSleeping"] = ui(0);
  art.properties["Name"] = string("native articulation");
  for (const auto *name : {"MaxProjectionIterations", "InternalDriveIterations",
                           "ExternalDriveIterations"})
    art.properties[name] = ui(3);
  art.properties["SolverIterationCounts.minPositionIters"] = ui(8);
  art.properties["SolverIterationCounts.minVelocityIters"] = ui(2);
  for (const auto *name : {"SeparationTolerance", "SleepThreshold",
                           "StabilizationThreshold", "WakeCounter"})
    art.properties[name] = floats("PvdF32", {.01f});
  f.state.instances.emplace(500, std::move(art));
  InstanceState joint;
  joint.id = 700;
  joint.class_name = {"physx3", "PxArticulationJoint"};
  joint.properties["Link"] = refs({101});
  joint.properties["ParentPose"] = joint.properties["ChildPose"] =
      floats("PxTransform", {0, 0, 0, 1, 0, 0, 0});
  joint.properties["TargetOrientation"] = floats("PxQuat", {0, 0, 0, 1});
  joint.properties["TargetVelocity"] = floats("PxVec3", {.2f, .3f, .4f});
  joint.properties["DriveType"] = ui(0);
  joint.properties["SwingLimitEnabled"] = ui(1);
  joint.properties["TwistLimitEnabled"] = ui(0);
  for (const auto *name :
       {"Stiffness", "Damping", "TangentialStiffness", "TangentialDamping",
        "InternalCompliance", "ExternalCompliance", "SwingLimitContactDistance",
        "TwistLimitContactDistance"})
    joint.properties[name] = floats("PvdF32", {.1f});
  joint.properties["SwingLimit.zLimit"] = floats("PvdF32", {.5f});
  joint.properties["SwingLimit.yLimit"] = floats("PvdF32", {.7f});
  joint.properties["TwistLimit.lower"] = floats("PvdF32", {-.4f});
  joint.properties["TwistLimit.upper"] = floats("PvdF32", {.6f});
  f.state.instances.emplace(700, std::move(joint));
  InstanceState aggregate;
  aggregate.id = 600;
  aggregate.class_name = {"physx3", "PxAggregate"};
  aggregate.properties["Scene"] = refs({1});
  aggregate.properties["Actors"] = refs({});
  aggregate.properties["Articulations"] = refs({500});
  aggregate.properties["MaxNbActors"] = ui(16);
  aggregate.properties["SelfCollision"] = ui(0);
  f.state.instances.emplace(600, std::move(aggregate));
  const auto result = xml::export_snapshot(f.metadata, f.state);
  check(result.complete && result.actor_count == 2 && result.shape_count == 2 &&
            result.articulation_count == 1 &&
            result.articulation_link_count == 2 &&
            result.articulation_joint_count == 1 && result.aggregate_count == 1,
        "native articulation unit graph/counters");
  check(result.objects.size() == 8 &&
            result.xml.find("<NumActors>2</NumActors>") != std::string::npos,
        "native object budget/membership");
  for (const auto &mapping : result.objects)
    if (mapping.type == "PxArticulationJoint")
      check(mapping.serial_id == 0 && mapping.owner_serial_id &&
                mapping.shape_index == UINT32_MAX,
            "native joint inline locator collides with first shape");
  check(xml::export_snapshot(f.metadata, big_endian(f.state)).xml == result.xml,
        "native unit BE values");
  xml::Options selected;
  selected.selection = xml::Selection::objects;
  selected.objects = {{101, 0}};
  rejects([&] { xml::export_snapshot(f.metadata, f.state, selected); });
  selected.unsupported = xml::UnsupportedPolicy::allow_partial;
  const auto omitted = xml::export_snapshot(f.metadata, f.state, selected);
  check(!omitted.complete && omitted.actor_count == 0 &&
            omitted.articulation_count == 0 &&
            omitted.xml.find("<PxArticulationLink>") == std::string::npos,
        "partial selection emitted orphan links");
  auto broken = f.state;
  broken.instances.at(101).properties["Links"] = refs({100});
  rejects([&] { xml::export_snapshot(f.metadata, broken); });
  broken = f.state;
  broken.instances.at(700).properties["DriveType"] = ui(1);
  broken.instances.at(700).properties["TargetOrientation"] =
      floats("PxQuat", {0, .2f, 0, 0});
  rejects([&] { xml::export_snapshot(f.metadata, broken); });
  broken.instances.at(700).properties["TargetOrientation"] =
      floats("PxQuat", {0, 1, 0, 0});
  check(xml::export_snapshot(f.metadata, broken).complete,
        "restorable eERROR unit target rejected");
  broken = f.state;
  broken.instances.at(101).properties["Mass"] = floats("PvdF32", {0});
  xml::Options partial;
  partial.unsupported = xml::UnsupportedPolicy::allow_partial;
  const auto bad_body = xml::export_snapshot(f.metadata, broken, partial);
  check(bad_body.articulation_count == 0 && bad_body.actor_count == 0,
        "bad link body left a partial articulation");
  broken = f.state;
  auto duplicate = broken.instances.at(600);
  duplicate.id = 601;
  broken.instances.emplace(601, std::move(duplicate));
  rejects([&] { xml::export_snapshot(f.metadata, broken); });
  const auto duplicate_aggregate =
      xml::export_snapshot(f.metadata, broken, partial);
  check(!duplicate_aggregate.complete &&
            duplicate_aggregate.aggregate_count == 1,
        "duplicate aggregate membership was silently serialized");
  xml::Options budget;
  budget.max_objects = 7;
  rejects([&] { xml::export_snapshot(f.metadata, f.state, budget); });
  budget.max_objects = 8;
  check(xml::export_snapshot(f.metadata, f.state, budget).objects.size() == 8,
        "native budget counts inline joint");
}
void real_articulations(const char *capture, const char *oracle_file,
                        const char *output) {
  auto session = Session::load_file(capture);
  std::size_t prefix = 0;
  for (const auto &section : session.sections())
    if (section.name == "articulation_oracle")
      prefix = section.event_count;
  check(prefix, "real native-articulation marker missing");
  const auto snapshot = session.snapshot_at_event(prefix);
  const auto &metadata = session.metadata();
  JointOracle oracle;
  std::ifstream file(std::filesystem::u8path(oracle_file));
  check(bool(file), "native SDK oracle CSV open");
  std::string line;
  std::size_t checked = 0;
  while (std::getline(file, line)) {
    auto fields = joint_csv_split(line);
    if (fields[0] == "step")
      continue;
    check(fields.size() == 8, "native oracle CSV schema");
    if (fields[0] != "1" || fields[1] != "post1" || fields[4][0] == '@')
      continue;
    if (fields[3] != "PxArticulation" && fields[3] != "PxArticulationJoint")
      continue;
    if (fields[6] != "f" && fields[6] != "u")
      continue;
    if (fields[4] == "IsSleeping")
      continue;
    const auto id = std::stoull(fields[2]);
    const auto *object = snapshot.instance(id);
    check(object, "native oracle object missing");
    JointOracleRow row{{},
                       fields[3],
                       fields[4],
                       fields[7],
                       static_cast<std::size_t>(std::stoull(fields[5])),
                       fields[6][0]};
    // This exact post-fetch oracle also covers the owner's synchronized
    // counter.
    {
      if (row.kind == 'f')
        check(std::abs(joint_float(*object, row) - std::stof(row.value)) <=
                  1e-6f,
              "native recorded joint/config field differs from SDK getter");
      else
        check(uint(*object, row.property.c_str()) == std::stoull(row.value),
              "native recorded integer differs from SDK getter");
      ++checked;
    }
    oracle[id].push_back(std::move(row));
  }
  check(oracle.size() == 6 && checked > 150,
        "native SDK property oracle coverage");
  const auto base = xml::export_snapshot(metadata, snapshot);
  check(base.complete && base.actor_count == 9 &&
            base.articulation_count == 2 && base.articulation_link_count == 6 &&
            base.articulation_joint_count == 4 && base.aggregate_count == 3 &&
            base.objects.size() == 28,
        "real native artifact/dependency coverage");
  emit_fixture(output, metadata, snapshot, {}, &oracle);
  const auto be = big_endian(snapshot);
  check(xml::export_snapshot(metadata, be).xml == base.xml,
        "native BE XML differs");
  emit_fixture(std::string(output) + "_big_endian", metadata, be, {}, &oracle);
  std::uint64_t main = 0;
  for (const auto &entry : snapshot.instances)
    if (entry.second.class_name.name == "PxArticulation" &&
        need(entry.second, "Name").text->find("Branched") != std::string::npos)
      main = entry.first;
  check(main, "branched native owner missing");
  const auto scene =
      need(*snapshot.instance(main), "Scene").as_object_ref().value();
  xml::Options scoped;
  scoped.scene = xml::ObjectKey{scene, snapshot.instance(scene)->generation};
  const auto scene_result = xml::export_snapshot(metadata, snapshot, scoped);
  check(scene_result.articulation_count == 1 &&
            scene_result.articulation_link_count == 4 &&
            scene_result.actor_count == 6 && scene_result.aggregate_count == 2,
        "native scene scope expanded across scenes");
  emit_fixture(std::string(output) + "_scene", metadata, snapshot, scoped,
               &oracle);
  xml::Options component;
  component.selection = xml::Selection::objects;
  component.scene = scoped.scene;
  for (const auto &entry : snapshot.instances)
    if (entry.second.class_name.name == "PxArticulationLink" &&
        articulation_owner(snapshot, entry.second) == main)
      component.objects.push_back({entry.first, entry.second.generation});
  const auto link_only = component;
  rejects([&] { xml::export_snapshot(metadata, snapshot, component); });
  for (const auto &entry : snapshot.instances)
    if (entry.second.class_name.name == "PxRigidDynamic" &&
        need(entry.second, "Name").text == "Moving aggregate member")
      component.objects.push_back({entry.first, entry.second.generation});
  const auto selected = xml::export_snapshot(metadata, snapshot, component);
  check(selected.actor_count == 5 && selected.aggregate_count == 1 &&
            selected.articulation_count == 1,
        "complete native component was not preserved");
  emit_fixture(std::string(output) + "_component", metadata, snapshot,
               component, &oracle);
  xml::Options attachments = component;
  attachments.selection = xml::Selection::attachments;
  attachments.objects.clear();
  for (const auto &actor : component.objects)
    for (auto id : unique_refs(*snapshot.instance(actor.id), "Shapes"))
      attachments.attachments.push_back(
          {*scoped.scene, actor, {id, snapshot.instance(id)->generation}});
  check(xml::export_snapshot(metadata, snapshot, attachments).xml ==
            selected.xml,
        "complete visible native attachments differ from selected component");
  emit_fixture(std::string(output) + "_visible", metadata, snapshot,
               attachments, &oracle);
  auto partial = link_only;
  partial.unsupported = xml::UnsupportedPolicy::allow_partial;
  const auto omitted_aggregate =
      xml::export_snapshot(metadata, snapshot, partial);
  check(!omitted_aggregate.complete &&
            omitted_aggregate.articulation_count == 1 &&
            omitted_aggregate.aggregate_count == 0 &&
            omitted_aggregate.actor_count == 4,
        "boundary partial included unselected aggregate actors");
  emit_fixture(std::string(output) + "_boundary_partial", metadata, snapshot,
               partial, &oracle);
  auto one = partial;
  one.objects.resize(1);
  const auto omitted_graph = xml::export_snapshot(metadata, snapshot, one);
  check(!omitted_graph.complete && omitted_graph.articulation_count == 0 &&
            omitted_graph.actor_count == 0,
        "partial link selection emitted orphan graph");
  xml::Options budget;
  budget.max_objects = 27;
  rejects([&] { xml::export_snapshot(metadata, snapshot, budget); });
  budget.max_objects = 28;
  check(xml::export_snapshot(metadata, snapshot, budget).objects.size() == 28,
        "native SDK object budget inconsistent");
  auto stale = component;
  ++stale.objects.front().generation;
  rejects([&] { xml::export_snapshot(metadata, snapshot, stale); });
  std::cout
      << "Native articulation SDK oracle: " << checked
      << " recorded fields, 2 graphs/6 links/4 joints/3 aggregates passed\n";
}
void real_joints(const char *capture, const char *oracle_file,
                 const char *output) {
  auto session = Session::load_file(capture);
  std::size_t prefix = 0;
  for (const auto &section : session.sections())
    if (section.name == "joint_oracle")
      prefix = section.event_count;
  check(prefix != 0, "real joint marker");
  const auto snapshot = session.snapshot_at_event(prefix);
  const auto &metadata = session.metadata();
  JointOracle oracle;
  std::ifstream file(std::filesystem::u8path(oracle_file));
  check(bool(file), "open independent joint getter oracle");
  std::string line;
  std::size_t rows = 0;
  while (std::getline(file, line)) {
    auto fields = joint_csv_split(line);
    if (fields[0] == "joint_name")
      continue;
    check(fields.size() == 6, "joint getter oracle CSV schema");
    JointOracleRow row{fields[0],
                       fields[1],
                       fields[2],
                       fields[5],
                       static_cast<std::size_t>(std::stoull(fields[3])),
                       fields[4][0]};
    const InstanceState *object = nullptr;
    for (const auto &entry : snapshot.instances)
      if (entry.second.class_name.name == row.type &&
          entry.second.property("Name") &&
          entry.second.property("Name")->text == row.name) {
        object = &entry.second;
        break;
      }
    check(object != nullptr, "SDK oracle joint exists in capture");
    if (row.kind == 'f')
      check(joint_float(*object, row) == std::stof(row.value),
            "PVD joint field differs from independent SDK getter float");
    else
      check(joint_uint(*object, row) == std::stoull(row.value),
            "PVD joint field differs from independent SDK getter integer/ref");
    oracle[object->id].push_back(std::move(row));
    ++rows;
  }
  check(rows == 243 && oracle.size() == 6,
        "six-type independent joint oracle coverage");
  const auto base = xml::export_snapshot(metadata, snapshot);
  check(base.complete && base.actor_count == 7 && base.joint_count == 6,
        "all six SDK joints were not exported");
  emit_fixture(output, metadata, snapshot, {}, &oracle);
  auto be = big_endian(snapshot);
  check(xml::export_snapshot(metadata, be).xml == base.xml,
        "LE/BE joint snapshot serialization differs");
  emit_fixture(std::string(output) + "_big_endian", metadata, be, {}, &oracle);
  std::map<std::string, std::uint64_t> by_name;
  for (const auto &entry : oracle)
    by_name[entry.second.front().name] = entry.first;
  const auto fixed = by_name.at("joint_fixed"),
             distance = by_name.at("joint_distance"),
             d6 = by_name.at("joint_d6");
  const auto endpoint = [&](std::uint64_t joint, int slot) {
    return *need(*snapshot.instance(joint),
                 slot ? "Actors.actor1" : "Actors.actor0")
                .as_object_ref();
  };
  xml::Options world;
  world.selection = xml::Selection::objects;
  const auto world_body = endpoint(fixed, 1);
  world.objects = {{world_body, snapshot.instance(world_body)->generation}};
  const auto scene_id =
      *need(*snapshot.instance(world_body), "Scene").as_object_ref();
  world.scene =
      xml::ObjectKey{scene_id, snapshot.instance(scene_id)->generation};
  const auto only_world = xml::export_snapshot(metadata, snapshot, world);
  check(only_world.actor_count == 1 && only_world.joint_count == 1,
        "world joint selection widened actors");
  emit_fixture(std::string(output) + "_world", metadata, snapshot, world,
               &oracle);
  xml::Options boundary;
  boundary.selection = xml::Selection::objects;
  boundary.scene = world.scene;
  const auto distance_body = endpoint(distance, 1),
             anchor = endpoint(distance, 0);
  boundary.objects = {
      {distance_body, snapshot.instance(distance_body)->generation}};
  bool crossed = false;
  try {
    xml::export_snapshot(metadata, snapshot, boundary);
  } catch (const xml::ExportError &e) {
    crossed = std::string(e.what()).find("boundary") != std::string::npos;
  }
  check(crossed,
        "selected actor cross-boundary joint was not explicitly rejected");
  boundary.unsupported = xml::UnsupportedPolicy::allow_partial;
  const auto partial = xml::export_snapshot(metadata, snapshot, boundary);
  check(!partial.complete && partial.actor_count == 1 &&
            partial.joint_count == 0 && !partial.diagnostics.empty(),
        "partial boundary joint silently widened or claimed complete");
  check(std::none_of(
            partial.objects.begin(), partial.objects.end(),
            [&](const auto &object) { return object.source.id == anchor; }),
        "unselected anchor leaked into partial export");
  emit_fixture(std::string(output) + "_boundary_partial", metadata, snapshot,
               boundary, &oracle);
  xml::Options closed;
  closed.selection = xml::Selection::objects;
  std::set<std::uint64_t> actors;
  for (const auto &entry : oracle) {
    const auto a = endpoint(entry.first, 0), b = endpoint(entry.first, 1);
    if (a && b) {
      actors.insert(a);
      actors.insert(b);
    }
  }
  for (auto actor : actors)
    closed.objects.push_back({actor, snapshot.instance(actor)->generation});
  const auto component = xml::export_snapshot(metadata, snapshot, closed);
  check(component.complete && component.actor_count == 5 &&
            component.joint_count == 4,
        "closed selected joint component was not preserved exactly");
  emit_fixture(std::string(output) + "_component", metadata, snapshot, closed,
               &oracle);
  xml::Options visible;
  visible.selection = xml::Selection::attachments;
  visible.scene = world.scene;
  for (auto actor : actors) {
    const auto &o = *snapshot.instance(actor);
    const auto sid = *need(o, "Scene").as_object_ref();
    for (auto shape : *need(o, "Shapes").reference_ids)
      visible.attachments.push_back(
          {{sid, snapshot.instance(sid)->generation},
           {actor, o.generation},
           {shape, snapshot.instance(shape)->generation}});
  }
  const auto attachments = xml::export_snapshot(metadata, snapshot, visible);
  check(
      attachments.actor_count == 5 && attachments.joint_count == 4 &&
          attachments.xml == component.xml,
      "visible attachments expanded actor boundary or dropped internal joints");
  emit_fixture(std::string(output) + "_visible", metadata, snapshot, visible,
               &oracle);
  auto broken = snapshot;
  broken.instances.at(fixed).properties["ConstraintFlags"] =
      ui(uint(*snapshot.instance(fixed), "ConstraintFlags") | 1);
  rejects([&] { xml::export_snapshot(metadata, broken); });
  xml::Options allow;
  allow.unsupported = xml::UnsupportedPolicy::allow_partial;
  const auto missing = xml::export_snapshot(metadata, broken, allow);
  check(!missing.complete && missing.joint_count == 5 &&
            missing.xml.find("<PxFixedJoint>") == std::string::npos,
        "read-only broken joint was fabricated as intact");
  emit_fixture(std::string(output) + "_broken_partial", metadata, broken, allow,
               &oracle);
  auto zero = snapshot;
  for (const auto &entry : oracle) {
    auto &properties = zero.instances.at(entry.first).properties;
    properties["ConstraintFlags"] = ui(0);
    for (const auto *field : {"DistanceJointFlags", "PrismaticJointFlags",
                              "RevoluteJointFlags", "SphericalJointFlags"})
      if (properties.count(field))
        properties[field] = ui(0);
    if (entry.first == d6)
      for (const auto *drive : {"eX", "eY", "eZ", "eSWING", "eTWIST", "eSLERP"})
        properties[std::string("Drive[") + drive + "].Flags"] = ui(0);
  }
  for (auto &entry : zero.instances) {
    auto &object = entry.second;
    if (object.property("ActorFlags"))
      object.properties["ActorFlags"] = ui(0);
    if (object.class_name.name == "PxShape" ||
        object.class_name.name == "PxMaterial")
      object.properties["Flags"] = ui(0);
  }
  const auto clear_flags = xml::export_snapshot(metadata, zero);
  check(clear_flags.xml.find("<ConstraintFlags>0</ConstraintFlags>") !=
            std::string::npos,
        "zero flags would retain SDK constructor defaults");
  emit_fixture(std::string(output) + "_zero_flags", metadata, zero, {},
               &oracle);
  auto invalid = snapshot;
  invalid.instances.at(d6).properties.erase("Drive[eSLERP].ForceLimit");
  rejects([&] { xml::export_snapshot(metadata, invalid); });
  check(xml::export_snapshot(metadata, invalid, allow).joint_count == 5,
        "missing drive data was silently defaulted");
  invalid = snapshot;
  invalid.instances.at(d6).properties["Motion[eX]"] = ui(99);
  rejects([&] { xml::export_snapshot(metadata, invalid); });
  invalid = snapshot;
  invalid.instances.at(fixed).properties["ConstraintFlags"] = ui(64);
  rejects([&] { xml::export_snapshot(metadata, invalid); });
  invalid = snapshot;
  invalid.instances.at(fixed).properties["ConstraintFlags"] = ui(512);
  rejects([&] { xml::export_snapshot(metadata, invalid); });
  invalid = snapshot;
  invalid.instances.at(d6).properties["SwingLimit.YAngle"] =
      floats("PvdF32", {0});
  rejects([&] { xml::export_snapshot(metadata, invalid); });
  invalid = snapshot;
  invalid.instances.at(fixed).properties["LocalPose[eACTOR0]"] =
      floats("PxTransform", {0, 0, 0, 0, 0, 0, 0});
  rejects([&] { xml::export_snapshot(metadata, invalid); });
  auto stale = world;
  stale.objects[0].generation++;
  rejects([&] { xml::export_snapshot(metadata, snapshot, stale); });
  xml::Options budget;
  budget.max_objects = 26;
  rejects([&] { xml::export_snapshot(metadata, snapshot, budget); });
  budget.max_objects = 27;
  check(xml::export_snapshot(metadata, snapshot, budget).joint_count == 6,
        "backing constraints were not charged consistently to object budget");
  auto renamed = snapshot;
  renamed.instances.at(fixed).properties["Name"] = string(u8"关节<&\" renamed");
  emit_fixture(std::string(output) + "_utf8", metadata, renamed, {}, &oracle);
  std::cout << "Joint SDK source oracle: " << rows
            << " fields, six kinds, boundary/partial/zero "
               "flags/generation/budget cases passed\n";
}

} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 1) {
      unit();
      native_articulation_unit();
    } else if (argc == 3)
      real(argv[1], argv[2]);
    else if (argc == 4) {
      std::ifstream oracle(std::filesystem::u8path(argv[2]));
      std::string header;
      std::getline(oracle, header);
      if (header.rfind("step,phase,object,class", 0) == 0)
        real_articulations(argv[1], argv[2], argv[3]);
      else
        real_joints(argv[1], argv[2], argv[3]);
    } else
      throw std::runtime_error("usage: repx_tests [capture.pxd2 output_prefix] "
                               "or [joints.pxd2 joints.csv output_prefix]");
    std::cout << "RepX tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "RepX tests failed: " << e.what() << '\n';
    return 1;
  }
}
