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
void expected(std::ostream &out, const Metadata &metadata,
              const Snapshot &state, const xml::Result &result,
              const xml::Options &options) {
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
    if (mapping.type == "PxRigidStatic" || mapping.type == "PxRigidDynamic") {
      c.s("Name", o.property("Name") ? need(o, "Name").text.value_or("") : "");
      c.pose("GlobalPose", pose(o, "GlobalPose"));
      for (const auto *p : {"ActorFlags", "DominanceGroup", "OwnerClient"})
        c.u(p, uint(o, p));
      if (o.property("ClientBehaviorFlags"))
        c.u("ClientBehaviorFlags", uint(o, "ClientBehaviorFlags"));
      auto refs = *need(o, "Shapes").reference_ids;
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
      if (mapping.type == "PxRigidDynamic") {
        c.pose("CMassLocalPose", pose(o, "CMassLocalPose"));
        c.f("Mass", scalar(o, "Mass"));
        c.vec("MassSpaceInertiaTensor", vec(o, "MassSpaceInertiaTensor"));
        const bool kinematic = (uint(o, "RigidBodyFlags") & 1) != 0;
        c.vec("LinearVelocity", kinematic ? Vec3{} : vec(o, "LinearVelocity"));
        c.vec("AngularVelocity",
              kinematic ? Vec3{} : vec(o, "AngularVelocity"));
        c.u("RigidBodyFlags", uint(o, "RigidBodyFlags"));
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
                  const Snapshot &snapshot, xml::Options options) {
  auto result = xml::export_snapshot(metadata, snapshot, options);
  xml::save_file(prefix + ".repx", result, true);
  std::ofstream csv(std::filesystem::u8path(prefix + "_expected.csv"));
  expected(csv, metadata, snapshot, result, options);
  std::cout << "RepX export: " << result.actor_count << " actors, "
            << result.shape_count << " shapes, " << result.resource_count
            << " resources, complete=" << result.complete << '\n';
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
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 1)
      unit();
    else if (argc == 3)
      real(argv[1], argv[2]);
    else
      throw std::runtime_error(
          "usage: repx_tests [capture.pxd2 output_prefix]");
    std::cout << "RepX tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "RepX tests failed: " << e.what() << '\n';
    return 1;
  }
}
