#include <PxPhysicsAPI.h>
#include <PxPvdDataStream.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace physx;
void need(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
std::uint64_t id(const void *p) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
}
struct Assertions : PxAssertHandler {
  void operator()(const char *e, const char *f, int n, bool &) override {
    std::fprintf(stderr, "SDK assertion: %s %s:%d\n", e, f, n);
    std::_Exit(3);
  }
};
struct Errors : PxErrorCallback {
  unsigned failures = 0;
  void reportError(PxErrorCode::Enum code, const char *m, const char *f,
                   int n) override {
    std::fprintf(stderr, "SDK [%u]: %s %s:%d\n", unsigned(code), m, f, n);
    if (code != PxErrorCode::eDEBUG_INFO && code != PxErrorCode::eDEBUG_WARNING)
      ++failures;
  }
};
struct Articulation {
  PxArticulation *value = nullptr;
  std::vector<PxArticulationLink *> links;
};
struct Context {
  PxDefaultAllocator allocator;
  Errors errors;
  PxFoundation *foundation = nullptr;
  PxPvd *pvd = nullptr;
  PxPvdTransport *transport = nullptr;
  PxPhysics *physics = nullptr;
  PxDefaultCpuDispatcher *dispatcher = nullptr;
  PxMaterial *material = nullptr;
  PxSerializationRegistry *registry = nullptr;
  bool extensions = false;
  std::array<PxScene *, 2> scenes{};
  std::array<PxAggregate *, 3> aggregates{};
  std::vector<PxRigidActor *> rigids;
  std::deque<std::string> names;
  Articulation main, other;
  const char *name(std::string v) {
    names.push_back(std::move(v));
    return names.back().c_str();
  }
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
    for (auto &scene : scenes) {
      scene = physics->createScene(desc);
      need(scene, "scene");
    }
    material = physics->createMaterial(.4f, .3f, .2f);
    need(material, "material");
  }
  void shape(PxRigidActor &actor, unsigned index) {
    auto *shape = physics->createShape(
        PxBoxGeometry(.2f + .03f * float(index), .25f, .3f), *material, true);
    need(shape, "shape");
    shape->setLocalPose(
        PxTransform(PxVec3(.03f, .02f, -.01f), PxQuat(.11f, PxVec3(1, 0, 0))));
    shape->setName(name("Shape " + std::to_string(index)));
    actor.attachShape(*shape);
    need(actor.getNbShapes() == 1, "attach shape");
    shape->release();
  }
  PxRigidActor *rigid(PxVec3 p, bool dynamic, const char *text) {
    PxRigidActor *actor =
        dynamic ? static_cast<PxRigidActor *>(physics->createRigidDynamic(
                      PxTransform(p, PxQuat(.2f, PxVec3(0, 1, 0)))))
                : static_cast<PxRigidActor *>(
                      physics->createRigidStatic(PxTransform(p)));
    need(actor, "rigid actor");
    rigids.push_back(actor);
    actor->setName(text);
    shape(*actor, 0);
    if (auto *body = actor->is<PxRigidDynamic>()) {
      body->setMass(2.3f);
      body->setMassSpaceInertiaTensor(PxVec3(.4f, .5f, .6f));
      body->setLinearVelocity(PxVec3(.2f, .1f, -.1f));
      body->setAngularVelocity(PxVec3(.05f, .1f, .02f));
    }
    return actor;
  }
  Articulation articulation(PxVec3 origin, const char *text, bool branched,
                            bool alternate = false) {
    Articulation a;
    a.value = physics->createArticulation();
    need(a.value, "articulation");
    a.value->setName(text);
    a.value->setMaxProjectionIterations(7);
    a.value->setSeparationTolerance(.012f);
    a.value->setInternalDriveIterations(5);
    a.value->setExternalDriveIterations(3);
    a.value->setSolverIterationCounts(8, 2);
    a.value->setSleepThreshold(.0001f);
    a.value->setStabilizationThreshold(.001f);
    a.value->setWakeCounter(.8f);
    const unsigned count = branched ? 4 : 2;
    const PxQuat rotation(.32f, PxVec3(0, 1, 0));
    for (unsigned i = 0; i < count; ++i) {
      const int parent = i == 0 ? -1 : i == 3 ? (alternate ? 2 : 1) : 0;
      const PxVec3 offset = i == 0   ? PxVec3(0)
                            : i == 1 ? PxVec3(-1.2f, 1.4f, 0)
                            : i == 2
                                ? PxVec3(1.3f, 1.5f, .1f)
                                : PxVec3(alternate ? 1.6f : -1.6f, 2.8f, .2f);
      auto *link = a.value->createLink(
          parent < 0 ? nullptr : a.links[static_cast<std::size_t>(parent)],
          PxTransform(origin + rotation.rotate(offset), rotation));
      need(link, "link");
      a.links.push_back(link);
      link->setName(name(std::string(text) + " link " + std::to_string(i)));
      shape(*link, i);
      link->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
      link->setMass(1.3f + .2f * float(i));
      link->setMassSpaceInertiaTensor(PxVec3(.4f, .5f, .6f) +
                                      PxVec3(.1f * float(i)));
      link->setCMassLocalPose(PxTransform(PxVec3(.02f, -.01f, .03f),
                                          PxQuat(.05f, PxVec3(0, 0, 1))));
      if (auto *j = link->getInboundJoint()) {
        const auto anchor =
            (a.links[static_cast<std::size_t>(parent)]->getGlobalPose().p +
             link->getGlobalPose().p) *
            .5f;
        j->setParentPose(PxTransform(a.links[static_cast<std::size_t>(parent)]
                                         ->getGlobalPose()
                                         .transformInv(anchor),
                                     PxQuat(.04f, PxVec3(1, 0, 0))));
        j->setChildPose(PxTransform(link->getGlobalPose().transformInv(anchor),
                                    PxQuat(.03f, PxVec3(0, 0, 1))));
        j->setDriveType(i == 2 ? PxArticulationJointDriveType::eERROR
                               : PxArticulationJointDriveType::eTARGET);
        j->setTargetOrientation(i == 2 ? PxQuat(0, 1, 0, 0)
                                       : PxQuat(.12f, PxVec3(0, 1, 0)));
        j->setTargetVelocity(PxVec3(.01f, .02f, .03f));
        j->setStiffness(.15f + float(i) * .03f);
        j->setDamping(.3f);
        j->setInternalCompliance(.6f);
        j->setExternalCompliance(.7f);
        j->setSwingLimit(.65f, .85f);
        j->setTangentialStiffness(.12f);
        j->setTangentialDamping(.14f);
        j->setSwingLimitContactDistance(.04f);
        j->setSwingLimitEnabled(true);
        j->setTwistLimit(-.35f, .45f);
        j->setTwistLimitContactDistance(.03f);
        j->setTwistLimitEnabled(true);
      }
    }
    return a;
  }
  ~Context() {
    if (main.value)
      main.value->release();
    if (other.value)
      other.value->release();
    for (auto *actor : rigids)
      if (actor)
        actor->release();
    for (auto *aggregate : aggregates)
      if (aggregate)
        aggregate->release();
    if (material)
      material->release();
    for (auto *scene : scenes)
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
  unsigned step = 0;
  std::string phase;
  std::uint64_t object = 0;
  const char *cls = "";
  explicit Csv(const std::filesystem::path &path)
      : out(path, std::ios::binary) {
    need(bool(out), "CSV output");
    out << std::setprecision(9)
        << "step,phase,object,class,property,component,type,value\n";
  }
  void prefix(const std::string &p, unsigned component, char type) {
    out << step << ',' << phase << ',' << object << ',' << cls << ',' << p
        << ',' << component << ',' << type << ',';
  }
  void u(const std::string &p, std::uint64_t value, unsigned component = 0,
         char type = 'u') {
    prefix(p, component, type);
    out << value << '\n';
  }
  void f(const std::string &p, float value, unsigned component = 0,
         char type = 'f') {
    prefix(p, component, type);
    out << value << '\n';
  }
  void r(const std::string &p, const void *value, unsigned component = 0) {
    u(p, id(value), component, 'r');
  }
  void s(const std::string &p, const char *text) {
    prefix(p, 0, 's');
    out << (text ? text : "") << '\n';
  }
  void vec(const std::string &p, PxVec3 v) {
    f(p, v.x);
    f(p, v.y, 1);
    f(p, v.z, 2);
  }
  void quat(const std::string &p, PxQuat q) {
    f(p, q.x);
    f(p, q.y, 1);
    f(p, q.z, 2);
    f(p, q.w, 3);
  }
  void pose(const std::string &p, PxTransform t) {
    f(p, t.q.x);
    f(p, t.q.y, 1);
    f(p, t.q.z, 2);
    f(p, t.q.w, 3);
    f(p, t.p.x, 4);
    f(p, t.p.y, 5);
    f(p, t.p.z, 6);
  }
  void begin(const PxBase &value) {
    object = id(&value);
    cls = value.getConcreteTypeName();
    u("@Present", 1);
  }
  template <class T> void refs(const char *p, const std::vector<T *> &values) {
    u(std::string(p) + ".@count", values.size(), 0, 'c');
    for (unsigned i = 0; i < values.size(); ++i)
      r(p, values[i], i);
  }
  void joint(PxArticulationJoint &j, PxArticulationLink &child) {
    begin(j);
    r("Link", &child);
    pose("ParentPose", j.getParentPose());
    pose("ChildPose", j.getChildPose());
    quat("TargetOrientation", j.getTargetOrientation());
    vec("TargetVelocity", j.getTargetVelocity());
    u("DriveType", j.getDriveType());
    f("Stiffness", j.getStiffness());
    f("Damping", j.getDamping());
    f("InternalCompliance", j.getInternalCompliance());
    f("ExternalCompliance", j.getExternalCompliance());
    PxReal a, b;
    j.getSwingLimit(a, b);
    f("SwingLimit.zLimit", a);
    f("SwingLimit.yLimit", b);
    f("TangentialStiffness", j.getTangentialStiffness());
    f("TangentialDamping", j.getTangentialDamping());
    f("SwingLimitContactDistance", j.getSwingLimitContactDistance());
    u("SwingLimitEnabled", j.getSwingLimitEnabled());
    j.getTwistLimit(a, b);
    f("TwistLimit.lower", a);
    f("TwistLimit.upper", b);
    u("TwistLimitEnabled", j.getTwistLimitEnabled());
    f("TwistLimitContactDistance", j.getTwistLimitContactDistance());
  }
  void body(PxRigidActor &actor) {
    begin(actor);
    s("Name", actor.getName());
    r("Scene", actor.getScene());
    r("Aggregate", actor.getAggregate());
    u("ActorFlags", PxU8(actor.getActorFlags()));
    u("DominanceGroup", actor.getDominanceGroup());
    u("OwnerClient", actor.getOwnerClient());
    u("ClientBehaviorFlags", PxU8(actor.getClientBehaviorFlags()));
    pose("GlobalPose", actor.getGlobalPose());
    if (auto *rb = actor.is<PxRigidBody>()) {
      pose("CMassLocalPose", rb->getCMassLocalPose());
      f("Mass", rb->getMass());
      f("InvMass", rb->getInvMass());
      vec("MassSpaceInertiaTensor", rb->getMassSpaceInertiaTensor());
      vec("MassSpaceInvInertiaTensor", rb->getMassSpaceInvInertiaTensor());
      vec("LinearVelocity", rb->getLinearVelocity());
      vec("AngularVelocity", rb->getAngularVelocity());
      u("RigidBodyFlags", PxU8(rb->getRigidBodyFlags()));
      f("MinCCDAdvanceCoefficient", rb->getMinCCDAdvanceCoefficient());
      f("MaxDepenetrationVelocity", rb->getMaxDepenetrationVelocity());
      f("MaxContactImpulse", rb->getMaxContactImpulse());
    }
    if (auto *rd = actor.is<PxRigidDynamic>()) {
      u("IsSleeping", rd->isSleeping());
      f("LinearDamping", rd->getLinearDamping());
      f("AngularDamping", rd->getAngularDamping());
    }
    std::vector<PxShape *> shapes(actor.getNbShapes());
    actor.getShapes(shapes.data(), static_cast<PxU32>(shapes.size()));
    refs("Shapes", shapes);
    for (auto *shape : shapes) {
      begin(*shape);
      s("Name", shape->getName());
      pose("LocalPose", shape->getLocalPose());
      u("Flags", PxU8(shape->getFlags()));
      u("IsExclusive", shape->isExclusive());
      f("ContactOffset", shape->getContactOffset());
      f("RestOffset", shape->getRestOffset());
      std::vector<PxMaterial *> materials(shape->getNbMaterials());
      shape->getMaterials(materials.data(),
                          static_cast<PxU32>(materials.size()));
      refs("Materials", materials);
      const auto world = actor.getGlobalPose() * shape->getLocalPose();
      pose("@WorldPose", world);
      r("@Actor", &actor);
      r("@Scene", actor.getScene());
      PxBoxGeometry box;
      need(shape->getBoxGeometry(box), "box geometry");
      vec("@HalfExtents", box.halfExtents);
      const auto bounds = PxGeometryQuery::getWorldBounds(box, world, 1);
      vec("@BoundsMin", bounds.minimum);
      vec("@BoundsMax", bounds.maximum);
    }
  }
  void articulation(Articulation &a) {
    begin(*a.value);
    s("Name", a.value->getName());
    r("Scene", a.value->getScene());
    r("Aggregate", a.value->getAggregate());
    u("MaxProjectionIterations", a.value->getMaxProjectionIterations());
    f("SeparationTolerance", a.value->getSeparationTolerance());
    u("InternalDriveIterations", a.value->getInternalDriveIterations());
    u("ExternalDriveIterations", a.value->getExternalDriveIterations());
    PxU32 p, v;
    a.value->getSolverIterationCounts(p, v);
    u("SolverIterationCounts.minPositionIters", p);
    u("SolverIterationCounts.minVelocityIters", v);
    u("IsSleeping", a.value->isSleeping());
    f("SleepThreshold", a.value->getSleepThreshold());
    f("StabilizationThreshold", a.value->getStabilizationThreshold());
    f("WakeCounter", a.value->getWakeCounter());
    std::vector<PxArticulationLink *> links(a.value->getNbLinks());
    a.value->getLinks(links.data(), static_cast<PxU32>(links.size()));
    std::vector<PxArticulationLink *> roots;
    std::map<PxArticulationLink *, PxArticulationLink *> parents;
    for (auto *link : links) {
      if (!link->getInboundJoint())
        roots.push_back(link);
      std::vector<PxArticulationLink *> children(link->getNbChildren());
      link->getChildren(children.data(), static_cast<PxU32>(children.size()));
      for (auto *child : children)
        parents[child] = link;
    }
    refs("Links", roots);
    refs("@AllLinks", links);
    for (auto *link : links) {
      body(*link);
      begin(*link);
      r("Parent", parents.count(link)
                      ? static_cast<const void *>(parents.at(link))
                      : static_cast<const void *>(a.value));
      r("InboundJoint", link->getInboundJoint());
      std::vector<PxArticulationLink *> children(link->getNbChildren());
      link->getChildren(children.data(), static_cast<PxU32>(children.size()));
      refs("Links", children);
      u("@Sleeping", a.value->isSleeping());
      if (auto *j = link->getInboundJoint())
        joint(*j, *link);
    }
  }
  void aggregate(PxAggregate &a) {
    begin(a);
    r("Scene", a.getScene());
    u("MaxNbActors", a.getMaxNbActors());
    u("SelfCollision", a.getSelfCollision());
    std::vector<PxActor *> all(a.getNbActors());
    a.getActors(all.data(), static_cast<PxU32>(all.size()));
    std::vector<PxActor *> actors;
    std::vector<PxArticulation *> articulations;
    for (auto *actor : all) {
      auto *link = actor->is<PxArticulationLink>();
      if (!link)
        actors.push_back(actor);
      else if (!link->getInboundJoint())
        articulations.push_back(&link->getArticulation());
    }
    refs("Actors", actors);
    refs("Articulations", articulations);
    refs("@AllActors", all);
  }
};
struct Output : PxOutputStream {
  std::ofstream out;
  explicit Output(const std::filesystem::path &p) : out(p, std::ios::binary) {
    need(bool(out), "XML output");
  }
  PxU32 write(const void *data, PxU32 n) override {
    out.write(static_cast<const char *>(data), n);
    return out ? n : 0;
  }
};
void official(Context &c, const std::filesystem::path &prefix) {
  auto *collection = PxCreateCollection();
  need(collection, "collection");
  struct Guard {
    PxCollection *p;
    ~Guard() { p->release(); }
  } guard{collection};
  for (auto *aggregate : c.aggregates)
    collection->add(*aggregate, id(aggregate));
  for (auto *actor : c.rigids)
    collection->add(*actor, id(actor));
  collection->add(*c.main.value, id(c.main.value));
  collection->add(*c.other.value, id(c.other.value));
  PxSerialization::complete(*collection, *c.registry);
  for (PxU32 i = 0; i < collection->getNbObjects(); ++i) {
    auto &object = collection->getObject(i);
    if (!collection->getId(object))
      collection->addId(object, id(&object));
  }
  Output output(prefix.string() + "_official.repx");
  PxSerialization::PxXmlMiscParameter misc;
  misc.upVector = PxVec3(0, 1, 0);
  misc.scale = c.physics->getTolerancesScale();
  need(PxSerialization::serializeCollectionToXml(
           output, *collection, *c.registry, nullptr, nullptr, &misc),
       "official XML serialization");
  output.out.flush();
  need(bool(output.out), "flush official XML");
}
void run(const std::filesystem::path &prefix) {
  if (!prefix.parent_path().empty())
    std::filesystem::create_directories(prefix.parent_path());
  Context c;
  c.init(prefix.string() + "_capture.pxd2");
  c.main = c.articulation(PxVec3(1, 5, 2), "Branched & articulation <A>", true);
  c.other = c.articulation(PxVec3(20, 6, -3), "Isolated articulation B", false);
  auto *actor = c.rigid(PxVec3(-6, 4, 2), true, "Moving aggregate member");
  auto *anchor =
      c.rigid(PxVec3(-12, 2, 1), false, "Self-collision aggregate anchor");
  auto *isolated = c.rigid(PxVec3(27, 4, 0), true, "Isolated rigid");
  c.aggregates[0] = c.physics->createAggregate(16, false);
  c.aggregates[1] = c.physics->createAggregate(16, true);
  c.aggregates[2] = c.physics->createAggregate(16, true);
  for (auto *a : c.aggregates)
    need(a, "aggregate");
  need(c.aggregates[0]->addArticulation(*c.main.value) &&
           c.aggregates[0]->addActor(*actor),
       "non-self aggregate members");
  need(c.aggregates[1]->addActor(*anchor), "self aggregate member");
  need(c.aggregates[2]->addArticulation(*c.other.value) &&
           c.aggregates[2]->addActor(*isolated),
       "isolated aggregate members");
  c.scenes[0]->addAggregate(*c.aggregates[0]);
  c.scenes[0]->addAggregate(*c.aggregates[1]);
  c.scenes[1]->addAggregate(*c.aggregates[2]);
  Csv csv(prefix.string() + "_expected.csv");
  auto *stream = pvdsdk::PvdDataStream::create(c.pvd);
  need(stream, "oracle marker stream");
  struct Guard {
    pvdsdk::PvdDataStream *p;
    ~Guard() { p->release(); }
  } guard{stream};
  auto record = [&](unsigned step, const char *phase) {
    csv.step = step;
    csv.phase = phase;
    csv.articulation(c.main);
    csv.articulation(c.other);
    for (auto *body : c.rigids)
      csv.body(*body);
    for (auto *a : c.aggregates)
      csv.aggregate(*a);
    need(bool(csv.out), "CSV write");
  };
  record(0, "initial");
  stream->beginSection(c.scenes[0], "articulation_initial");
  stream->endSection(c.scenes[0], "articulation_initial");
  for (unsigned step = 1; step <= 10; ++step) {
    if (step == 2)
      need(c.aggregates[0]->removeActor(*actor), "remove aggregate member");
    if (step == 3) {
      c.scenes[0]->removeActor(*actor);
      need(c.aggregates[1]->addActor(*actor), "move actor to other aggregate");
    }
    if (step == 4)
      c.main.value->putToSleep();
    if (step == 5)
      c.main.value->wakeUp();
    if (step == 6) {
      need(c.aggregates[0]->removeArticulation(*c.main.value),
           "remove articulation aggregate membership");
      c.scenes[0]->removeArticulation(*c.main.value);
      need(c.aggregates[1]->addArticulation(*c.main.value),
           "reparent articulation to self aggregate");
    }
    if (step == 7) {
      c.main.value->release();
      c.main = {};
      c.main = c.articulation(PxVec3(2, 7, 3), "Recreated branch articulation",
                              true, true);
      need(c.aggregates[1]->addArticulation(*c.main.value),
           "recreate articulation aggregate member");
    }
    if (step == 8) {
      c.scenes[0]->removeAggregate(*c.aggregates[1]);
      c.scenes[1]->addAggregate(*c.aggregates[1]);
    }
    if (step == 9) {
      c.aggregates[0]->release();
      c.aggregates[0] = c.physics->createAggregate(8, false);
      need(c.aggregates[0], "recreate empty aggregate");
      c.scenes[1]->addAggregate(*c.aggregates[0]);
    }
    for (unsigned scene = 0; scene < 2; ++scene) {
      record(step, scene ? "frame1" : "frame0");
      c.scenes[scene]->simulate(1.f / 60);
      need(c.scenes[scene]->fetchResults(true), "fetch articulation scene");
      record(step, scene ? "post1" : "post0");
      stream->beginSection(c.scenes[scene], "validation_after_fetch");
      stream->endSection(c.scenes[scene], "validation_after_fetch");
    }
    if (step == 1) {
      stream->beginSection(c.scenes[1], "articulation_oracle");
      stream->endSection(c.scenes[1], "articulation_oracle");
      official(c, prefix);
    }
  }
  csv.out.flush();
  need(bool(csv.out), "flush expected CSV");
  need(!c.errors.failures, "SDK reported errors");
  std::cout << "Articulation/aggregate SDK fixture generated: "
            << prefix.string()
            << "_{capture.pxd2,expected.csv,official.repx}\n";
}
} // namespace
int main(int argc, char **argv) {
  Assertions assertions;
  PxSetAssertHandler(assertions);
  try {
    need(argc == 2, "usage: pvd_physx34_articulation_capture output_prefix");
    run(argv[1]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
