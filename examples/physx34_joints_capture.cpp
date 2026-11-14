#include <PxPhysicsAPI.h>
#include <PxPvdDataStream.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace physx;
void need(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::uint64_t id(const void *p) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
}
struct Assertions : PxAssertHandler {
  void operator()(const char *expression, const char *file, int line,
                  bool &) override {
    std::cerr << "SDK assertion: " << expression << ' ' << file << ':' << line
              << '\n';
    std::_Exit(3);
  }
};
struct Errors : PxErrorCallback {
  unsigned failures = 0;
  void reportError(PxErrorCode::Enum code, const char *message,
                   const char *file, int line) override {
    std::cerr << "SDK [" << unsigned(code) << "] " << message << ' ' << file
              << ':' << line << '\n';
    if (code != PxErrorCode::eDEBUG_INFO && code != PxErrorCode::eDEBUG_WARNING)
      ++failures;
  }
};
struct Context {
  PxDefaultAllocator allocator;
  Errors errors;
  PxFoundation *foundation = nullptr;
  PxPvd *pvd = nullptr;
  PxPvdTransport *transport = nullptr;
  PxPhysics *physics = nullptr;
  PxDefaultCpuDispatcher *dispatcher = nullptr;
  PxScene *scene = nullptr;
  PxMaterial *material = nullptr;
  PxSerializationRegistry *registry = nullptr;
  std::vector<PxJoint *> joints;
  std::vector<PxRigidActor *> actors;
  bool extensions = false;
  void init(const std::string &path) {
    foundation = PxCreateFoundation(PX_FOUNDATION_VERSION, allocator, errors);
    need(foundation, "foundation");
    pvd = PxCreatePvd(*foundation);
    need(pvd, "PVD");
    transport = PxDefaultPvdFileTransportCreate(path.c_str());
    need(transport, "transport");
    need(pvd->connect(*transport, PxPvdInstrumentationFlag::eDEBUG), "connect");
    physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation,
                              PxTolerancesScale(), true, pvd);
    need(physics, "physics");
    extensions = PxInitExtensions(*physics, pvd);
    need(extensions, "extensions");
    registry = PxSerialization::createSerializationRegistry(*physics);
    need(registry, "registry");
    dispatcher = PxDefaultCpuDispatcherCreate(0);
    need(dispatcher, "dispatcher");
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    scene = physics->createScene(desc);
    need(scene, "scene");
    scene->setVisualizationParameter(PxVisualizationParameter::eSCALE, 1);
    scene->setVisualizationParameter(
        PxVisualizationParameter::eJOINT_LOCAL_FRAMES, 1);
    scene->setVisualizationParameter(PxVisualizationParameter::eJOINT_LIMITS,
                                     1);
    if (auto *client = scene->getScenePvdClient())
      client->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONSTRAINTS, true);
    material = physics->createMaterial(.4f, .3f, .2f);
    need(material, "material");
  }
  PxRigidActor *actor(PxVec3 position, bool dynamic, const char *name) {
    PxRigidActor *value =
        dynamic ? static_cast<PxRigidActor *>(
                      physics->createRigidDynamic(PxTransform(position)))
                : static_cast<PxRigidActor *>(
                      physics->createRigidStatic(PxTransform(position)));
    need(value, "actor");
    actors.push_back(value);
    value->setName(name);
    auto *shape =
        physics->createShape(PxBoxGeometry(.3f, .4f, .5f), *material, true);
    need(shape, "shape");
    value->attachShape(*shape);
    need(value->getNbShapes() == 1, "attach");
    shape->release();
    scene->addActor(*value);
    return value;
  }
  ~Context() {
    for (auto *j : joints)
      j->release();
    for (auto *a : actors)
      a->release();
    if (material)
      material->release();
    if (scene)
      scene->release();
    if (dispatcher)
      dispatcher->release();
    if (registry)
      registry->release();
    if (extensions)
      PxCloseExtensions();
    if (physics)
      physics->release();
    if (pvd) {
      pvd->disconnect();
      pvd->release();
    }
    if (transport)
      transport->release();
    if (foundation)
      foundation->release();
  }
};
struct Csv {
  std::ofstream out;
  const PxJoint *joint = nullptr;
  std::size_t fields = 0;
  explicit Csv(const std::filesystem::path &path)
      : out(path, std::ios::binary) {
    need(bool(out), "oracle output");
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "joint_name,class,property,index,type,value\n";
  }
  void prefix(const std::string &p, unsigned index, char type) {
    out << joint->getName() << ',' << joint->getConcreteTypeName() << ',' << p
        << ',' << index << ',' << type << ',';
    ++fields;
  }
  void f(const std::string &p, float v, unsigned index = 0) {
    prefix(p, index, 'f');
    out << v << '\n';
  }
  void u(const std::string &p, std::uint64_t v, unsigned index = 0) {
    prefix(p, index, 'u');
    out << v << '\n';
  }
  template <class E, class T>
  void u(const std::string &p, PxFlags<E, T> v, unsigned index = 0) {
    u(p, static_cast<T>(v), index);
  }
  void vec(const std::string &p, PxVec3 v) {
    f(p, v.x, 0);
    f(p, v.y, 1);
    f(p, v.z, 2);
  }
  void pose(const std::string &p, PxTransform t) {
    f(p, t.q.x, 0);
    f(p, t.q.y, 1);
    f(p, t.q.z, 2);
    f(p, t.q.w, 3);
    f(p, t.p.x, 4);
    f(p, t.p.y, 5);
    f(p, t.p.z, 6);
  }
  void limit(const std::string &p, const PxJointLimitParameters &v) {
    f(p + ".Restitution", v.restitution);
    f(p + ".BounceThreshold", v.bounceThreshold);
    f(p + ".Stiffness", v.stiffness);
    f(p + ".Damping", v.damping);
    f(p + ".ContactDistance", v.contactDistance);
  }
  void angular(const std::string &p, const PxJointAngularLimitPair &v) {
    limit(p, v);
    f(p + ".Upper", v.upper);
    f(p + ".Lower", v.lower);
  }
  void cone(const std::string &p, const PxJointLimitCone &v) {
    limit(p, v);
    f(p + ".YAngle", v.yAngle);
    f(p + ".ZAngle", v.zAngle);
  }
  void record(PxJoint &j) {
    joint = &j;
    PxRigidActor *a, *b;
    j.getActors(a, b);
    u("Actors.actor0", id(a));
    u("Actors.actor1", id(b));
    pose("LocalPose[eACTOR0]", j.getLocalPose(PxJointActorIndex::eACTOR0));
    pose("LocalPose[eACTOR1]", j.getLocalPose(PxJointActorIndex::eACTOR1));
    PxReal force, torque;
    j.getBreakForce(force, torque);
    f("BreakForce.force", force);
    f("BreakForce.torque", torque);
    u("ConstraintFlags", j.getConstraintFlags());
    f("InvMassScale0", j.getInvMassScale0());
    f("InvMassScale1", j.getInvMassScale1());
    f("InvInertiaScale0", j.getInvInertiaScale0());
    f("InvInertiaScale1", j.getInvInertiaScale1());
    if (auto *v = j.is<PxFixedJoint>()) {
      f("ProjectionLinearTolerance", v->getProjectionLinearTolerance());
      f("ProjectionAngularTolerance", v->getProjectionAngularTolerance());
    }
    if (auto *v = j.is<PxDistanceJoint>()) {
      f("MinDistance", v->getMinDistance());
      f("MaxDistance", v->getMaxDistance());
      f("Tolerance", v->getTolerance());
      f("Stiffness", v->getStiffness());
      f("Damping", v->getDamping());
      u("DistanceJointFlags", v->getDistanceJointFlags());
    }
    if (auto *v = j.is<PxPrismaticJoint>()) {
      auto l = v->getLimit();
      limit("Limit", l);
      f("Limit.Upper", l.upper);
      f("Limit.Lower", l.lower);
      u("PrismaticJointFlags", v->getPrismaticJointFlags());
      f("ProjectionLinearTolerance", v->getProjectionLinearTolerance());
      f("ProjectionAngularTolerance", v->getProjectionAngularTolerance());
    }
    if (auto *v = j.is<PxRevoluteJoint>()) {
      angular("Limit", v->getLimit());
      f("DriveVelocity", v->getDriveVelocity());
      f("DriveForceLimit", v->getDriveForceLimit());
      f("DriveGearRatio", v->getDriveGearRatio());
      u("RevoluteJointFlags", v->getRevoluteJointFlags());
      f("ProjectionLinearTolerance", v->getProjectionLinearTolerance());
      f("ProjectionAngularTolerance", v->getProjectionAngularTolerance());
    }
    if (auto *v = j.is<PxSphericalJoint>()) {
      cone("LimitCone", v->getLimitCone());
      u("SphericalJointFlags", v->getSphericalJointFlags());
      f("ProjectionLinearTolerance", v->getProjectionLinearTolerance());
    }
    if (auto *v = j.is<PxD6Joint>()) {
      const char *axes[] = {"eX", "eY", "eZ", "eTWIST", "eSWING1", "eSWING2"};
      const char *drives[] = {"eX", "eY", "eZ", "eSWING", "eTWIST", "eSLERP"};
      for (unsigned i = 0; i < 6; ++i) {
        u(std::string("Motion[") + axes[i] + "]",
          v->getMotion(static_cast<PxD6Axis::Enum>(i)));
        auto d = v->getDrive(static_cast<PxD6Drive::Enum>(i));
        auto p = std::string("Drive[") + drives[i] + "]";
        f(p + ".Stiffness", d.stiffness);
        f(p + ".Damping", d.damping);
        f(p + ".ForceLimit", d.forceLimit);
        u(p + ".Flags", d.flags);
      }
      auto l = v->getLinearLimit();
      limit("LinearLimit", l);
      f("LinearLimit.Value", l.value);
      angular("TwistLimit", v->getTwistLimit());
      cone("SwingLimit", v->getSwingLimit());
      pose("DrivePosition", v->getDrivePosition());
      PxVec3 linear, angular_v;
      v->getDriveVelocity(linear, angular_v);
      vec("DriveVelocity.linear", linear);
      vec("DriveVelocity.angular", angular_v);
      f("ProjectionLinearTolerance", v->getProjectionLinearTolerance());
      f("ProjectionAngularTolerance", v->getProjectionAngularTolerance());
    }
  }
};
template <class T> T tuned(T value) {
  value.restitution = .25f;
  value.bounceThreshold = .6f;
  value.stiffness = 13.f;
  value.damping = 2.5f;
  return value;
}
struct Output : PxOutputStream {
  std::ofstream out;
  explicit Output(const std::filesystem::path &p) : out(p, std::ios::binary) {
    need(bool(out), "official XML output");
  }
  PxU32 write(const void *bytes, PxU32 count) override {
    out.write(static_cast<const char *>(bytes), count);
    return out ? count : 0;
  }
};
void run(const std::filesystem::path &prefix) {
  if (!prefix.parent_path().empty())
    std::filesystem::create_directories(prefix.parent_path());
  Context c;
  c.init(prefix.string() + "_capture.pxd2");
  auto *anchor = c.actor(PxVec3(0, 3, 0), false, "joint_anchor");
  const std::array<const char *, 6> names = {
      "joint_fixed",    "joint_distance",  "joint_prismatic",
      "joint_revolute", "joint_spherical", "joint_d6"};
  for (unsigned i = 0; i < 6; ++i) {
    auto *body = c.actor(PxVec3(float(i) * 3, 3, 1), true, names[i]);
    PxTransform a(PxVec3(.2f, .3f, .4f), PxQuat(.1f, PxVec3(0, 1, 0))),
        b(PxVec3(-.1f, .2f, .3f), PxQuat(-.15f, PxVec3(1, 0, 0)));
    PxJoint *joint = nullptr;
    switch (i) {
    case 0: {
      auto *v = PxFixedJointCreate(*c.physics, nullptr, a, body, b);
      v->setProjectionLinearTolerance(.7f);
      v->setProjectionAngularTolerance(.2f);
      joint = v;
      break;
    }
    case 1: {
      auto *v = PxDistanceJointCreate(*c.physics, anchor, a, body, b);
      v->setMinDistance(.2f);
      v->setMaxDistance(7.f);
      v->setTolerance(.03f);
      v->setStiffness(31.f);
      v->setDamping(4.f);
      v->setDistanceJointFlags(PxDistanceJointFlag::eMIN_DISTANCE_ENABLED |
                               PxDistanceJointFlag::eMAX_DISTANCE_ENABLED |
                               PxDistanceJointFlag::eSPRING_ENABLED);
      joint = v;
      break;
    }
    case 2: {
      auto *v = PxPrismaticJointCreate(*c.physics, anchor, a, body, b);
      v->setLimit(tuned(PxJointLinearLimitPair(c.physics->getTolerancesScale(),
                                               -2.f, 4.f, .1f)));
      v->setPrismaticJointFlag(PxPrismaticJointFlag::eLIMIT_ENABLED, true);
      v->setProjectionLinearTolerance(.8f);
      v->setProjectionAngularTolerance(.3f);
      joint = v;
      break;
    }
    case 3: {
      auto *v = PxRevoluteJointCreate(*c.physics, anchor, a, body, b);
      v->setLimit(tuned(PxJointAngularLimitPair(-.4f, .8f, .05f)));
      v->setDriveVelocity(.6f);
      v->setDriveForceLimit(100.f);
      v->setDriveGearRatio(1.3f);
      v->setRevoluteJointFlags(PxRevoluteJointFlag::eLIMIT_ENABLED |
                               PxRevoluteJointFlag::eDRIVE_ENABLED |
                               PxRevoluteJointFlag::eDRIVE_FREESPIN);
      v->setProjectionLinearTolerance(.9f);
      v->setProjectionAngularTolerance(.4f);
      joint = v;
      break;
    }
    case 4: {
      auto *v = PxSphericalJointCreate(*c.physics, nullptr, a, body, b);
      v->setLimitCone(tuned(PxJointLimitCone(.5f, .7f, .03f)));
      v->setSphericalJointFlag(PxSphericalJointFlag::eLIMIT_ENABLED, true);
      v->setProjectionLinearTolerance(1.1f);
      joint = v;
      break;
    }
    case 5: {
      auto *v = PxD6JointCreate(*c.physics, anchor, a, body, b);
      for (unsigned k = 0; k < 6; ++k) {
        v->setMotion(static_cast<PxD6Axis::Enum>(k),
                     static_cast<PxD6Motion::Enum>(k % 3));
        v->setDrive(static_cast<PxD6Drive::Enum>(k),
                    PxD6JointDrive(11.f + float(k), 2.f + float(k),
                                   90.f + float(k), k % 2 == 0));
      }
      v->setLinearLimit(
          tuned(PxJointLinearLimit(c.physics->getTolerancesScale(), 2.f, .1f)));
      v->setTwistLimit(tuned(PxJointAngularLimitPair(-.5f, .8f, .04f)));
      v->setSwingLimit(tuned(PxJointLimitCone(.7f, .9f, .03f)));
      v->setDrivePosition(
          PxTransform(PxVec3(.1f, .2f, .3f), PxQuat(.2f, PxVec3(0, 0, 1))));
      v->setDriveVelocity(PxVec3(.1f, .2f, .3f), PxVec3(.4f, .5f, .6f));
      v->setProjectionLinearTolerance(.6f);
      v->setProjectionAngularTolerance(.3f);
      joint = v;
      break;
    }
    }
    need(joint, "joint create");
    c.joints.push_back(joint);
    joint->setName(names[i]);
    joint->setBreakForce(10000000.f + float(i), 20000000.f + float(i) * 2);
    joint->setInvMassScale0(.7f);
    joint->setInvMassScale1(.8f);
    joint->setInvInertiaScale0(.9f);
    joint->setInvInertiaScale1(1.2f);
    joint->setConstraintFlag(PxConstraintFlag::eVISUALIZATION, true);
    joint->setConstraintFlag(PxConstraintFlag::eCOLLISION_ENABLED, true);
  }
  c.scene->simulate(1.f / 60);
  need(c.scene->fetchResults(true), "fetch");
  auto *stream = pvdsdk::PvdDataStream::create(c.pvd);
  need(stream, "marker stream");
  stream->beginSection(c.scene, "joint_oracle");
  stream->endSection(c.scene, "joint_oracle");
  stream->release();
  Csv csv(prefix.string() + "_joints.csv");
  for (auto *joint : c.joints)
    csv.record(*joint);
  csv.out.flush();
  need(bool(csv.out), "flush oracle");
  auto *collection = PxCreateCollection();
  need(collection, "collection");
  struct Guard {
    PxCollection *p;
    ~Guard() { p->release(); }
  } guard{collection};
  for (auto *actor : c.actors)
    collection->add(*actor, id(actor));
  for (auto *joint : c.joints)
    collection->add(*joint, id(joint));
  PxSerialization::complete(*collection, *c.registry);
  for (PxU32 i = 0; i < collection->getNbObjects(); ++i) {
    auto &o = collection->getObject(i);
    if (!collection->getId(o))
      collection->addId(o, id(&o));
  }
  Output output(prefix.string() + "_official.repx");
  PxSerialization::PxXmlMiscParameter misc;
  misc.upVector = PxVec3(0, 1, 0);
  misc.scale = c.physics->getTolerancesScale();
  need(PxSerialization::serializeCollectionToXml(
           output, *collection, *c.registry, nullptr, nullptr, &misc),
       "official serialization");
  output.out.flush();
  need(bool(output.out), "flush official XML");
  need(!c.errors.failures, "SDK errors");
  std::cout << "Six-joint SDK fixture: " << csv.fields << " getter fields, "
            << collection->getNbObjects() << " official objects\n";
}
} // namespace
int main(int argc, char **argv) {
  Assertions assertions;
  PxSetAssertHandler(assertions);
  try {
    need(argc == 2, "usage: pvd_physx34_joints_capture output_prefix");
    run(argv[1]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
