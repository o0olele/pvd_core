// Isolated /MTd SDK process. No pvd_core or pvd::repx linkage: files are the
// only boundary between the independent exporter and the SDK XML loader.
#include <PxPhysicsAPI.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace physx;
void need(bool b, const std::string &message) {
  if (!b)
    throw std::runtime_error(message);
}
struct Assertions : PxAssertHandler {
  void operator()(const char *e, const char *f, int l, bool &) override {
    std::fprintf(stderr, "PhysX assertion: %s (%s:%d)\n", e, f, l);
    std::fflush(nullptr);
    std::_Exit(3);
  }
};
struct Errors : PxErrorCallback {
  unsigned failures = 0;
  void reportError(PxErrorCode::Enum code, const char *text, const char *file,
                   int line) override {
    std::fprintf(stderr, "PhysX [%u] %s (%s:%d)\n", unsigned(code), text, file,
                 line);
    if (code != PxErrorCode::eDEBUG_INFO && code != PxErrorCode::eDEBUG_WARNING)
      ++failures;
  }
};
struct Context {
  PxDefaultAllocator allocator;
  Errors errors;
  PxFoundation *foundation = nullptr;
  PxPhysics *physics = nullptr;
  PxCooking *cooking = nullptr;
  PxSerializationRegistry *registry = nullptr;
  PxStringTable *strings = nullptr;
  Context() {
    foundation = PxCreateFoundation(PX_FOUNDATION_VERSION, allocator, errors);
    need(foundation, "foundation");
    physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation,
                              PxTolerancesScale(), true);
    need(physics, "physics");
    cooking = PxCreateCooking(PX_PHYSICS_VERSION, *foundation,
                              PxCookingParams(physics->getTolerancesScale()));
    need(cooking, "cooking");
    registry = PxSerialization::createSerializationRegistry(*physics);
    need(registry, "registry");
    strings = &PxStringTableExt::createStringTable(allocator);
  }
  ~Context() {
    if (registry)
      registry->release();
    if (cooking)
      cooking->release();
    if (physics)
      physics->release();
    if (strings)
      strings->release();
    if (foundation)
      foundation->release();
  }
};
struct Input : PxInputData {
  std::vector<PxU8> data;
  PxU32 position = 0;
  explicit Input(const std::string &path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    need(bool(file), "open RepX XML");
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    need(size >= 0 && static_cast<std::uint64_t>(size) <= 256u * 1024u * 1024u,
         "XML input size");
    data.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(data.data()), size);
    need(bool(file), "read RepX XML");
  }
  PxU32 read(void *output, PxU32 count) override {
    const auto n = std::min<PxU32>(count, getLength() - position);
    if (n)
      std::memcpy(output, data.data() + position, n);
    position += n;
    return n;
  }
  PxU32 getLength() const override { return static_cast<PxU32>(data.size()); }
  void seek(PxU32 offset) override { position = std::min(offset, getLength()); }
  PxU32 tell() const override { return position; }
};
std::string float_text(float value) {
  char b[64];
  const auto r =
      std::to_chars(b, b + sizeof(b), value, std::chars_format::general,
                    std::numeric_limits<float>::max_digits10);
  need(r.ec == std::errc{}, "float output");
  return std::string(b, r.ptr);
}
std::string hex(const char *text) {
  std::string r;
  const char *digits = "0123456789abcdef";
  if (text)
    for (const auto *p = reinterpret_cast<const unsigned char *>(text); *p;
         ++p) {
      r += digits[*p >> 4];
      r += digits[*p & 15];
    }
  return r;
}
struct Locator {
  std::uint64_t serial = 0, owner = 0;
  std::size_t shape = 0;
  bool operator<(const Locator &o) const {
    return std::tie(serial, owner, shape) <
           std::tie(o.serial, o.owner, o.shape);
  }
};
struct Field {
  char type = 'u';
  std::string value;
};
using Fields = std::map<std::pair<std::string, std::size_t>, Field>;
void u(Fields &f, const std::string &name, std::uint64_t value,
       std::size_t index = 0) {
  f[{name, index}] = {'u', std::to_string(value)};
}
template <class Enum, class Storage>
void u(Fields &f, const std::string &name, PxFlags<Enum, Storage> value,
       std::size_t index = 0) {
  u(f, name, static_cast<Storage>(value), index);
}
void scalar(Fields &f, const std::string &name, float value,
            std::size_t index = 0) {
  f[{name, index}] = {'f', float_text(value)};
}
void vec(Fields &f, const std::string &name, const PxVec3 &v,
         std::size_t index = 0) {
  scalar(f, name, v.x, index);
  scalar(f, name, v.y, index + 1);
  scalar(f, name, v.z, index + 2);
}
void quat(Fields &f, const std::string &name, const PxQuat &q,
          std::size_t index = 0) {
  scalar(f, name, q.x, index);
  scalar(f, name, q.y, index + 1);
  scalar(f, name, q.z, index + 2);
  scalar(f, name, q.w, index + 3);
}
void pose(Fields &f, const std::string &name, const PxTransform &p,
          std::size_t index = 0) {
  quat(f, name, p.q, index);
  vec(f, name, p.p, index + 4);
}
bool less(const PxVec3 &a, const PxVec3 &b) {
  return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
}
void vertices(Fields &fields, std::vector<PxVec3> points) {
  std::sort(points.begin(), points.end(), less);
  u(fields, "VertexCount", points.size());
  for (std::size_t i = 0; i < points.size(); ++i)
    vec(fields, "Vertex", points[i], i * 3);
}
void triangles(Fields &fields, const PxTriangleMesh &mesh) {
  std::vector<std::array<PxVec3, 3>> all;
  const bool small =
      bool(mesh.getTriangleMeshFlags() & PxTriangleMeshFlag::e16_BIT_INDICES);
  for (PxU32 i = 0; i < mesh.getNbTriangles(); ++i) {
    std::array<PxVec3, 3> triangle;
    for (PxU32 j = 0; j < 3; ++j) {
      const auto index =
          small ? static_cast<const PxU16 *>(mesh.getTriangles())[i * 3 + j]
                : static_cast<const PxU32 *>(mesh.getTriangles())[i * 3 + j];
      triangle[j] = mesh.getVertices()[index];
    }
    const auto minimum = static_cast<std::size_t>(
        std::min_element(triangle.begin(), triangle.end(), less) -
        triangle.begin());
    std::rotate(triangle.begin(), triangle.begin() + minimum, triangle.end());
    all.push_back(triangle);
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
      vec(fields, "TriangleVertex", all[i][j], i * 9 + j * 3);
    const auto normal =
        (all[i][1] - all[i][0]).cross(all[i][2] - all[i][0]).getNormalized();
    vec(fields, "TriangleNormal", normal, i * 3);
  }
}
Fields inspect(PxBase &object, PxCollection &collection,
               const std::map<PxSerialObjectId, PxSerialObjectId> &aliases) {
  const auto serial = [&](const PxBase &value) {
    const auto raw = collection.getId(value);
    const auto found = aliases.find(raw);
    return found == aliases.end() ? raw : found->second;
  };
  Fields f;
  if (auto *actor = object.is<PxRigidActor>()) {
    f[{"Name", 0}] = {'s', hex(actor->getName())};
    pose(f, "GlobalPose", actor->getGlobalPose());
    u(f, "ActorFlags", actor->getActorFlags());
    u(f, "DominanceGroup", actor->getDominanceGroup());
    u(f, "OwnerClient", actor->getOwnerClient());
    u(f, "ClientBehaviorFlags", actor->getClientBehaviorFlags());
    std::vector<PxShape *> shapes(actor->getNbShapes());
    actor->getShapes(shapes.data(), static_cast<PxU32>(shapes.size()));
    u(f, "ShapeCount", shapes.size());
    for (std::size_t i = 0; i < shapes.size(); ++i) {
      u(f, "ShapeRef", serial(*shapes[i]), i);
      pose(f, "ShapeWorldPose",
           actor->getGlobalPose() * shapes[i]->getLocalPose(), i * 7);
    }
    if (auto *body = actor->is<PxRigidBody>()) {
      pose(f, "CMassLocalPose", body->getCMassLocalPose());
      scalar(f, "Mass", body->getMass());
      vec(f, "MassSpaceInertiaTensor", body->getMassSpaceInertiaTensor());
      vec(f, "LinearVelocity", body->getLinearVelocity());
      vec(f, "AngularVelocity", body->getAngularVelocity());
      u(f, "RigidBodyFlags", body->getRigidBodyFlags());
      scalar(f, "MinCCDAdvanceCoefficient",
             body->getMinCCDAdvanceCoefficient());
      scalar(f, "MaxDepenetrationVelocity",
             body->getMaxDepenetrationVelocity());
      scalar(f, "MaxContactImpulse", body->getMaxContactImpulse());
    }
    if (auto *dynamic = actor->is<PxRigidDynamic>()) {
      pose(f, "CMassLocalPose", dynamic->getCMassLocalPose());
      scalar(f, "Mass", dynamic->getMass());
      vec(f, "MassSpaceInertiaTensor", dynamic->getMassSpaceInertiaTensor());
      vec(f, "LinearVelocity", dynamic->getLinearVelocity());
      vec(f, "AngularVelocity", dynamic->getAngularVelocity());
      u(f, "RigidBodyFlags", dynamic->getRigidBodyFlags());
      PxTransform target(PxIdentity);
      u(f, "HasKinematicTarget", dynamic->getKinematicTarget(target) ? 1 : 0);
      u(f, "RigidDynamicLockFlags", dynamic->getRigidDynamicLockFlags());
      scalar(f, "LinearDamping", dynamic->getLinearDamping());
      scalar(f, "AngularDamping", dynamic->getAngularDamping());
      scalar(f, "MaxAngularVelocity", dynamic->getMaxAngularVelocity());
      scalar(f, "MinCCDAdvanceCoefficient",
             dynamic->getMinCCDAdvanceCoefficient());
      scalar(f, "MaxDepenetrationVelocity",
             dynamic->getMaxDepenetrationVelocity());
      scalar(f, "MaxContactImpulse", dynamic->getMaxContactImpulse());
      scalar(f, "SleepThreshold", dynamic->getSleepThreshold());
      scalar(f, "StabilizationThreshold", dynamic->getStabilizationThreshold());
      scalar(f, "WakeCounter", dynamic->getWakeCounter());
      scalar(f, "ContactReportThreshold", dynamic->getContactReportThreshold());
      PxU32 p = 0, v = 0;
      dynamic->getSolverIterationCounts(p, v);
      u(f, "minPositionIters", p);
      u(f, "minVelocityIters", v);
    }
  }
  if (auto *joint = object.is<PxJoint>()) {
    f[{"Name", 0}] = {'s', hex(joint->getName())};
    PxRigidActor *a0 = nullptr;
    PxRigidActor *a1 = nullptr;
    joint->getActors(a0, a1);
    u(f, "Actors.actor0", a0 ? serial(*a0) : 0);
    u(f, "Actors.actor1", a1 ? serial(*a1) : 0);
    pose(f, "LocalPose[eACTOR0]",
         joint->getLocalPose(PxJointActorIndex::eACTOR0));
    pose(f, "LocalPose[eACTOR1]",
         joint->getLocalPose(PxJointActorIndex::eACTOR1));
    PxReal force = 0, torque = 0;
    joint->getBreakForce(force, torque);
    scalar(f, "BreakForce.force", force);
    scalar(f, "BreakForce.torque", torque);
    u(f, "ConstraintFlags", joint->getConstraintFlags());
    scalar(f, "InvMassScale0", joint->getInvMassScale0());
    scalar(f, "InvMassScale1", joint->getInvMassScale1());
    scalar(f, "InvInertiaScale0", joint->getInvInertiaScale0());
    scalar(f, "InvInertiaScale1", joint->getInvInertiaScale1());
    const auto limit = [&](const std::string &p,
                           const PxJointLimitParameters &l) {
      scalar(f, p + ".Restitution", l.restitution);
      scalar(f, p + ".BounceThreshold", l.bounceThreshold);
      scalar(f, p + ".Stiffness", l.stiffness);
      scalar(f, p + ".Damping", l.damping);
      scalar(f, p + ".ContactDistance", l.contactDistance);
    };
    const auto pair = [&](const std::string &p, const auto &l) {
      limit(p, l);
      scalar(f, p + ".Lower", l.lower);
      scalar(f, p + ".Upper", l.upper);
    };
    const auto cone = [&](const std::string &p, const PxJointLimitCone &l) {
      limit(p, l);
      scalar(f, p + ".YAngle", l.yAngle);
      scalar(f, p + ".ZAngle", l.zAngle);
    };
    if (auto *typed = joint->is<PxFixedJoint>()) {
      scalar(f, "ProjectionLinearTolerance",
             typed->getProjectionLinearTolerance());
      scalar(f, "ProjectionAngularTolerance",
             typed->getProjectionAngularTolerance());
    }
    if (auto *typed = joint->is<PxDistanceJoint>()) {
      scalar(f, "MinDistance", typed->getMinDistance());
      scalar(f, "MaxDistance", typed->getMaxDistance());
      scalar(f, "Tolerance", typed->getTolerance());
      scalar(f, "Stiffness", typed->getStiffness());
      scalar(f, "Damping", typed->getDamping());
      u(f, "DistanceJointFlags", typed->getDistanceJointFlags());
    }
    if (auto *typed = joint->is<PxPrismaticJoint>()) {
      pair("Limit", typed->getLimit());
      u(f, "PrismaticJointFlags", typed->getPrismaticJointFlags());
      scalar(f, "ProjectionLinearTolerance",
             typed->getProjectionLinearTolerance());
      scalar(f, "ProjectionAngularTolerance",
             typed->getProjectionAngularTolerance());
    }
    if (auto *typed = joint->is<PxRevoluteJoint>()) {
      pair("Limit", typed->getLimit());
      scalar(f, "DriveVelocity", typed->getDriveVelocity());
      scalar(f, "DriveForceLimit", typed->getDriveForceLimit());
      scalar(f, "DriveGearRatio", typed->getDriveGearRatio());
      u(f, "RevoluteJointFlags", typed->getRevoluteJointFlags());
      scalar(f, "ProjectionLinearTolerance",
             typed->getProjectionLinearTolerance());
      scalar(f, "ProjectionAngularTolerance",
             typed->getProjectionAngularTolerance());
    }
    if (auto *typed = joint->is<PxSphericalJoint>()) {
      cone("LimitCone", typed->getLimitCone());
      u(f, "SphericalJointFlags", typed->getSphericalJointFlags());
      scalar(f, "ProjectionLinearTolerance",
             typed->getProjectionLinearTolerance());
    }
    if (auto *typed = joint->is<PxD6Joint>()) {
      const char *axes[] = {"eX", "eY", "eZ", "eTWIST", "eSWING1", "eSWING2"};
      const char *drives[] = {"eX", "eY", "eZ", "eSWING", "eTWIST", "eSLERP"};
      for (PxU32 i = 0; i < 6; ++i) {
        u(f, std::string("Motion[") + axes[i] + "]",
          static_cast<PxU32>(typed->getMotion(static_cast<PxD6Axis::Enum>(i))));
        const auto d = typed->getDrive(static_cast<PxD6Drive::Enum>(i));
        const auto p = std::string("Drive[") + drives[i] + "]";
        scalar(f, p + ".Stiffness", d.stiffness);
        scalar(f, p + ".Damping", d.damping);
        scalar(f, p + ".ForceLimit", d.forceLimit);
        u(f, p + ".Flags", d.flags);
      }
      const auto linear = typed->getLinearLimit();
      limit("LinearLimit", linear);
      scalar(f, "LinearLimit.Value", linear.value);
      pair("TwistLimit", typed->getTwistLimit());
      cone("SwingLimit", typed->getSwingLimit());
      pose(f, "DrivePosition", typed->getDrivePosition());
      PxVec3 v, w;
      typed->getDriveVelocity(v, w);
      vec(f, "DriveVelocity.linear", v);
      vec(f, "DriveVelocity.angular", w);
      scalar(f, "ProjectionLinearTolerance",
             typed->getProjectionLinearTolerance());
      scalar(f, "ProjectionAngularTolerance",
             typed->getProjectionAngularTolerance());
    }
  }
  if (auto *art = object.is<PxArticulation>()) {
    f[{"Name", 0}] = {'s', hex(art->getName())};
    u(f, "MaxProjectionIterations", art->getMaxProjectionIterations());
    u(f, "InternalDriveIterations", art->getInternalDriveIterations());
    u(f, "ExternalDriveIterations", art->getExternalDriveIterations());
    PxU32 position = 0, velocity = 0;
    art->getSolverIterationCounts(position, velocity);
    u(f, "SolverIterationCounts.minPositionIters", position);
    u(f, "SolverIterationCounts.minVelocityIters", velocity);
    scalar(f, "SeparationTolerance", art->getSeparationTolerance());
    scalar(f, "SleepThreshold", art->getSleepThreshold());
    scalar(f, "StabilizationThreshold", art->getStabilizationThreshold());
    scalar(f, "WakeCounter", art->getWakeCounter());
    std::vector<PxArticulationLink *> links(art->getNbLinks());
    art->getLinks(links.data(), static_cast<PxU32>(links.size()));
    std::vector<PxSerialObjectId> ids;
    for (auto *link : links)
      ids.push_back(serial(*link));
    std::sort(ids.begin(), ids.end());
    u(f, "LinkCount", ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i)
      u(f, "LinkRef", ids[i], i);
  }
  if (auto *link = object.is<PxArticulationLink>()) {
    auto &art = link->getArticulation();
    u(f, "ArticulationRef", serial(art));
    std::vector<PxArticulationLink *> links(art.getNbLinks());
    art.getLinks(links.data(), static_cast<PxU32>(links.size()));
    PxSerialObjectId parent = 0;
    for (auto *candidate : links) {
      std::vector<PxArticulationLink *> children(candidate->getNbChildren());
      candidate->getChildren(children.data(),
                             static_cast<PxU32>(children.size()));
      if (std::find(children.begin(), children.end(), link) != children.end())
        parent = serial(*candidate);
    }
    u(f, "ParentRef", parent);
    std::vector<PxArticulationLink *> children(link->getNbChildren());
    link->getChildren(children.data(), static_cast<PxU32>(children.size()));
    std::vector<PxSerialObjectId> ids;
    for (auto *child : children)
      ids.push_back(serial(*child));
    std::sort(ids.begin(), ids.end());
    u(f, "ChildCount", ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i)
      u(f, "ChildRef", ids[i], i);
  }
  if (auto *joint = object.is<PxArticulationJoint>()) {
    pose(f, "ParentPose", joint->getParentPose());
    pose(f, "ChildPose", joint->getChildPose());
    quat(f, "TargetOrientation", joint->getTargetOrientation());
    vec(f, "TargetVelocity", joint->getTargetVelocity());
    u(f, "DriveType", static_cast<PxU32>(joint->getDriveType()));
    scalar(f, "Stiffness", joint->getStiffness());
    scalar(f, "Damping", joint->getDamping());
    scalar(f, "InternalCompliance", joint->getInternalCompliance());
    scalar(f, "ExternalCompliance", joint->getExternalCompliance());
    PxReal first = 0, second = 0;
    joint->getSwingLimit(first, second);
    scalar(f, "SwingLimit.zLimit", first);
    scalar(f, "SwingLimit.yLimit", second);
    joint->getTwistLimit(first, second);
    scalar(f, "TwistLimit.lower", first);
    scalar(f, "TwistLimit.upper", second);
    scalar(f, "TangentialStiffness", joint->getTangentialStiffness());
    scalar(f, "TangentialDamping", joint->getTangentialDamping());
    scalar(f, "SwingLimitContactDistance",
           joint->getSwingLimitContactDistance());
    scalar(f, "TwistLimitContactDistance",
           joint->getTwistLimitContactDistance());
    u(f, "SwingLimitEnabled", joint->getSwingLimitEnabled());
    u(f, "TwistLimitEnabled", joint->getTwistLimitEnabled());
  }
  if (auto *aggregate = object.is<PxAggregate>()) {
    u(f, "MaxNbActors", aggregate->getMaxNbActors());
    u(f, "SelfCollision", aggregate->getSelfCollision());
    std::vector<PxActor *> actors(aggregate->getNbActors());
    aggregate->getActors(actors.data(), static_cast<PxU32>(actors.size()));
    std::vector<PxSerialObjectId> ids;
    for (auto *actor : actors)
      ids.push_back(serial(*actor));
    std::sort(ids.begin(), ids.end());
    u(f, "ActorCount", ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i)
      u(f, "ActorRef", ids[i], i);
  }
  if (auto *shape = object.is<PxShape>()) {
    f[{"Name", 0}] = {'s', hex(shape->getName())};
    pose(f, "LocalPose", shape->getLocalPose());
    u(f, "IsExclusive", shape->isExclusive() ? 1 : 0);
    u(f, "Flags", shape->getFlags());
    scalar(f, "ContactOffset", shape->getContactOffset());
    scalar(f, "RestOffset", shape->getRestOffset());
    const auto sim = shape->getSimulationFilterData(),
               query = shape->getQueryFilterData();
    const PxU32 simwords[] = {sim.word0, sim.word1, sim.word2, sim.word3},
                querywords[] = {query.word0, query.word1, query.word2,
                                query.word3};
    for (std::size_t i = 0; i < 4; ++i) {
      u(f, "SimulationFilterData", simwords[i], i);
      u(f, "QueryFilterData", querywords[i], i);
    }
    std::vector<PxMaterial *> mats(shape->getNbMaterials());
    shape->getMaterials(mats.data(), static_cast<PxU32>(mats.size()));
    u(f, "MaterialCount", mats.size());
    for (std::size_t i = 0; i < mats.size(); ++i)
      u(f, "MaterialRef", serial(*mats[i]), i);
    const auto g = shape->getGeometry();
    u(f, "GeometryType", g.getType());
    switch (g.getType()) {
    case PxGeometryType::eBOX:
      vec(f, "HalfExtents", g.box().halfExtents);
      break;
    case PxGeometryType::eSPHERE:
      scalar(f, "Radius", g.sphere().radius);
      break;
    case PxGeometryType::eCAPSULE:
      scalar(f, "Radius", g.capsule().radius);
      scalar(f, "HalfHeight", g.capsule().halfHeight);
      break;
    case PxGeometryType::eCONVEXMESH:
      vec(f, "MeshScale", g.convexMesh().scale.scale);
      quat(f, "MeshRotation", g.convexMesh().scale.rotation);
      u(f, "MeshFlags", g.convexMesh().meshFlags);
      u(f, "ResourceRef", serial(*g.convexMesh().convexMesh));
      break;
    case PxGeometryType::eTRIANGLEMESH:
      vec(f, "MeshScale", g.triangleMesh().scale.scale);
      quat(f, "MeshRotation", g.triangleMesh().scale.rotation);
      u(f, "MeshFlags", g.triangleMesh().meshFlags);
      u(f, "ResourceRef", serial(*g.triangleMesh().triangleMesh));
      break;
    case PxGeometryType::eHEIGHTFIELD:
      scalar(f, "HeightScale", g.heightField().heightScale);
      scalar(f, "RowScale", g.heightField().rowScale);
      scalar(f, "ColumnScale", g.heightField().columnScale);
      u(f, "MeshFlags", g.heightField().heightFieldFlags);
      u(f, "ResourceRef", serial(*g.heightField().heightField));
      break;
    case PxGeometryType::ePLANE:
      break;
    default:
      throw std::runtime_error("unexpected loaded geometry");
    }
  }
  if (auto *material = object.is<PxMaterial>()) {
    scalar(f, "StaticFriction", material->getStaticFriction());
    scalar(f, "DynamicFriction", material->getDynamicFriction());
    scalar(f, "Restitution", material->getRestitution());
    u(f, "Flags", material->getFlags());
    u(f, "FrictionCombineMode", material->getFrictionCombineMode());
    u(f, "RestitutionCombineMode", material->getRestitutionCombineMode());
  }
  if (auto *mesh = object.is<PxTriangleMesh>()) {
    vertices(f,
             std::vector<PxVec3>(mesh->getVertices(),
                                 mesh->getVertices() + mesh->getNbVertices()));
    u(f, "TriangleCount", mesh->getNbTriangles());
    triangles(f, *mesh);
  }
  if (auto *mesh = object.is<PxConvexMesh>())
    vertices(f,
             std::vector<PxVec3>(mesh->getVertices(),
                                 mesh->getVertices() + mesh->getNbVertices()));
  if (auto *hf = object.is<PxHeightField>()) {
    u(f, "NbRows", hf->getNbRows());
    u(f, "NbColumns", hf->getNbColumns());
    u(f, "Format", hf->getFormat());
    u(f, "Flags", hf->getFlags());
    scalar(f, "Thickness", hf->getThickness());
    scalar(f, "ConvexEdgeThreshold", hf->getConvexEdgeThreshold());
    std::vector<PxHeightFieldSample> samples(std::size_t(hf->getNbRows()) *
                                             hf->getNbColumns());
    need(hf->saveCells(samples.data(),
                       static_cast<PxU32>(samples.size() *
                                          sizeof(PxHeightFieldSample))) ==
             samples.size() * sizeof(PxHeightFieldSample),
         "save heightfield cells");
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const auto *b = reinterpret_cast<const PxU8 *>(&samples[i]);
      const auto packed = PxU32(b[0]) | (PxU32(b[1]) << 8) |
                          (PxU32(b[2]) << 16) | (PxU32(b[3]) << 24);
      u(f, "Sample", packed, i);
    }
  }
  return f;
}
std::vector<std::string> split(const std::string &s) {
  std::vector<std::string> row;
  std::size_t start = 0;
  for (;;) {
    const auto at = s.find(',', start);
    row.push_back(s.substr(start, at - start));
    if (at == std::string::npos)
      break;
    start = at + 1;
  }
  return row;
}
struct LinkPath {
  PxSerialObjectId articulation = 0;
  std::vector<PxU32> children;
};
using LinkPaths = std::map<PxSerialObjectId, LinkPath>;
LinkPaths link_paths(PxCollection &collection) {
  LinkPaths result;
  for (PxU32 i = 0; i < collection.getNbObjects(); ++i)
    if (auto *art = collection.getObject(i).is<PxArticulation>()) {
      std::vector<PxArticulationLink *> all(art->getNbLinks());
      art->getLinks(all.data(), static_cast<PxU32>(all.size()));
      std::vector<std::pair<PxArticulationLink *, LinkPath>> queue;
      for (auto *link : all)
        if (!link->getInboundJoint())
          queue.push_back({link, {collection.getId(*art), {}}});
      need(queue.size() == 1, "SDK articulation root topology");
      for (std::size_t at = 0; at < queue.size(); ++at) {
        auto *link = queue[at].first;
        const auto path = queue[at].second;
        result.emplace(collection.getId(*link), path);
        std::vector<PxArticulationLink *> children(link->getNbChildren());
        link->getChildren(children.data(), static_cast<PxU32>(children.size()));
        for (PxU32 child = 0; child < children.size(); ++child) {
          auto next = path;
          next.children.push_back(child);
          queue.push_back({children[child], std::move(next)});
        }
      }
      need(queue.size() == all.size(), "SDK articulation covers all links");
    }
  return result;
}
PxArticulationLink *resolve_link(PxCollection &collection,
                                 const LinkPath &path) {
  auto *base = collection.find(path.articulation);
  need(base && base->is<PxArticulation>(),
       "official-cycle articulation stable ID");
  auto *art = base->is<PxArticulation>();
  std::vector<PxArticulationLink *> all(art->getNbLinks());
  art->getLinks(all.data(), static_cast<PxU32>(all.size()));
  PxArticulationLink *current = nullptr;
  for (auto *link : all)
    if (!link->getInboundJoint())
      current = link;
  need(current, "official-cycle root link");
  for (auto index : path.children) {
    std::vector<PxArticulationLink *> children(current->getNbChildren());
    current->getChildren(children.data(), static_cast<PxU32>(children.size()));
    need(index < children.size(), "official-cycle child path");
    current = children[index];
  }
  return current;
}
void validate(const std::string &xml, const std::string &expected,
              const std::string &oracle = {},
              const LinkPaths *prior_links = nullptr,
              LinkPaths *retained_links = nullptr) {
  Context context;
  Input input(xml);
  auto *collection = PxSerialization::createCollectionFromXml(
      input, *context.cooking, *context.registry, nullptr, context.strings);
  need(collection, "SDK createCollectionFromXml failed");
  struct Guard {
    PxCollection *collection;
    ~Guard() { collection->release(); }
  } guard{collection};
  std::ifstream csv(std::filesystem::u8path(expected));
  need(bool(csv), "open expected CSV");
  std::map<PxSerialObjectId, PxBase *> remapped_objects;
  std::map<PxSerialObjectId, PxSerialObjectId> aliases;
  if (prior_links)
    for (const auto &entry : *prior_links) {
      auto *link = resolve_link(*collection, entry.second);
      remapped_objects.emplace(entry.first, link);
      aliases.emplace(collection->getId(*link), entry.first);
    }
  const auto locate = [&](PxSerialObjectId id) -> PxBase * {
    const auto found = remapped_objects.find(id);
    return found == remapped_objects.end() ? collection->find(id)
                                           : found->second;
  };
  std::map<Locator, Fields> actual;
  std::set<PxBase *> covered;
  std::size_t fields = 0, objects = 0;
  std::string line;
  while (std::getline(csv, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto row = split(line);
    if (row.empty() || row[0] == "kind")
      continue;
    need(row.size() >= 5, "expected CSV schema");
    const Locator key{std::stoull(row[1]), std::stoull(row[2]),
                      static_cast<std::size_t>(std::stoull(row[3]))};
    if (row[0] == "object") {
      PxBase *object = nullptr;
      if (key.serial)
        object = locate(key.serial);
      else {
        auto *owner = locate(key.owner);
        if (key.shape == UINT32_MAX) {
          need(owner && owner->is<PxArticulationLink>(),
               "inline articulation joint owner");
          object = owner->is<PxArticulationLink>()->getInboundJoint();
        } else {
          need(owner && owner->is<PxRigidActor>(), "inline shape owner");
          PxShape *shape = nullptr;
          need(owner->is<PxRigidActor>()->getShapes(
                   &shape, 1, static_cast<PxU32>(key.shape)) == 1,
               "inline shape index");
          object = shape;
        }
      }
      need(object, "mapped SDK object exists");
      const auto &type = row[4];
      need(
          (type == "PxShape" && object->is<PxShape>()) ||
              (type == "PxArticulation" && object->is<PxArticulation>()) ||
              (type == "PxArticulationLink" &&
               object->is<PxArticulationLink>()) ||
              (type == "PxArticulationJoint" &&
               object->is<PxArticulationJoint>()) ||
              (type == "PxAggregate" && object->is<PxAggregate>()) ||
              (type == object->getConcreteTypeName() &&
               object->is<PxJoint>()) ||
              (type == "PxRigidStatic" && object->is<PxRigidStatic>()) ||
              (type == "PxRigidDynamic" && object->is<PxRigidDynamic>()) ||
              (type == "PxMaterial" && object->is<PxMaterial>()) ||
              (type == "PxBVH33TriangleMesh" && object->is<PxTriangleMesh>()) ||
              (type == "PxConvexMesh" && object->is<PxConvexMesh>()) ||
              (type == "PxHeightField" && object->is<PxHeightField>()),
          "SDK concrete type matches mapping");
      actual.emplace(key, inspect(*object, *collection, aliases));
      covered.insert(object);
      if (auto *joint = object->is<PxJoint>()) {
        need(joint->getConstraint() != nullptr,
             "joint backing constraint created");
        covered.insert(joint->getConstraint());
      }
      ++objects;
      continue;
    }
    need(row.size() == 7 && actual.count(key),
         "expected field after mapped object");
    const auto found = actual.at(key).find(
        {row[4], static_cast<std::size_t>(std::stoull(row[5]))});
    need(found != actual.at(key).end(), "SDK field available: " + row[4]);
    const auto &value = found->second;
    need(value.type == row[0][0], "SDK field type");
    if (value.type == 'f') {
      const auto a = std::stod(value.value), b = std::stod(row[6]);
      need(std::isfinite(a) && std::isfinite(b) &&
               std::abs(a - b) <= 1e-5 * std::max(1., std::abs(b)),
           "SDK float mismatch: object " + std::to_string(key.serial) + " " +
               row[4] + " actual=" + value.value + " expected=" + row[6]);
    } else
      need(value.value == row[6], "SDK exact field mismatch: " + row[4] +
                                      " actual=" + value.value +
                                      " expected=" + row[6]);
    ++fields;
  }
  need(covered.size() == collection->getNbObjects(),
       "every loaded object independently covered");
  need(!context.errors.failures, "SDK reported load errors");
  std::cout << "RepX SDK validation passed: " << covered.size() << " objects ("
            << objects << " mapped), " << fields << " fields\n";
  if (retained_links)
    *retained_links = link_paths(*collection);
  if (!oracle.empty()) {
    struct Output : PxOutputStream {
      std::ofstream stream;
      explicit Output(const std::string &path)
          : stream(std::filesystem::u8path(path), std::ios::binary) {}
      PxU32 write(const void *data, PxU32 size) override {
        stream.write(static_cast<const char *>(data), size);
        return stream ? size : 0;
      }
    } output(oracle);
    need(bool(output.stream), "open official SDK oracle output");
    PxSerialization::PxXmlMiscParameter misc;
    misc.upVector = PxVec3(0, 1, 0);
    misc.scale = context.physics->getTolerancesScale();
    need(PxSerialization::serializeCollectionToXml(
             output, *collection, *context.registry, nullptr, nullptr, &misc),
         "official SDK XML serialization");
    output.stream.flush();
    need(bool(output.stream), "flush official oracle XML");
  }
}
} // namespace
int main(int argc, char **argv) {
  Assertions assertions;
  PxSetAssertHandler(assertions);
  try {
    need(argc == 3 || (argc == 5 && std::string(argv[3]) == "--oracle"),
         "usage: physx34_repx_validate file.repx expected.csv [--oracle "
         "output.repx]");
    LinkPaths links;
    validate(argv[1], argv[2], argc == 5 ? argv[4] : "", nullptr, &links);
    if (argc == 5)
      validate(argv[4], argv[2], {}, &links);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "RepX SDK validation failed: " << e.what() << '\n';
    return 1;
  }
}
