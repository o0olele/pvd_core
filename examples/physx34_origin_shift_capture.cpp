#include <PxPhysicsAPI.h>
#include <PxPvdDataStream.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <extensions/PxClothFabricCooker.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
  void operator()(const char *e, const char *f, int line, bool &) override {
    std::fprintf(stderr, "SDK assertion: %s %s:%d\n", e, f, line);
    std::_Exit(3);
  }
};
struct Errors : PxErrorCallback {
  unsigned failures = 0;
  void reportError(PxErrorCode::Enum code, const char *m, const char *f,
                   int line) override {
    std::fprintf(stderr, "SDK [%u]: %s %s:%d\n", unsigned(code), m, f, line);
    if (code != PxErrorCode::eDEBUG_INFO && code != PxErrorCode::eDEBUG_WARNING)
      ++failures;
  }
};
struct Csv {
  std::ofstream out;
  std::string phase;
  std::uint64_t scene = 0, object = 0;
  std::string name, kind;
  std::size_t fields = 0;
  explicit Csv(const std::filesystem::path &path)
      : out(path, std::ios::binary) {
    need(bool(out), "oracle output");
    out << "phase,scene_id,object_id,name,kind,property,item,component,value\n"
        << std::setprecision(9);
  }
  void scalar(const std::string &property, std::size_t item,
              const char *component, double value) {
    out << phase << ',' << scene << ',' << object << ',' << name << ',' << kind
        << ',' << property << ',' << item << ',' << component << ',' << value
        << '\n';
    ++fields;
  }
  void vec(const std::string &property, const PxVec3 &v, std::size_t item = 0) {
    scalar(property, item, "x", v.x);
    scalar(property, item, "y", v.y);
    scalar(property, item, "z", v.z);
  }
  void pose(const std::string &property, const PxTransform &v) {
    scalar(property, 0, "px", v.p.x);
    scalar(property, 0, "py", v.p.y);
    scalar(property, 0, "pz", v.p.z);
    scalar(property, 0, "qx", v.q.x);
    scalar(property, 0, "qy", v.q.y);
    scalar(property, 0, "qz", v.q.z);
    scalar(property, 0, "qw", v.q.w);
  }
  void actor(const PxRigidActor &a) {
    object = id(&a);
    name = a.getName() ? a.getName() : "";
    kind = a.getConcreteTypeName();
    pose("GlobalPose", a.getGlobalPose());
    if (const auto *body = a.is<PxRigidBody>()) {
      vec("LinearVelocity", body->getLinearVelocity());
      vec("AngularVelocity", body->getAngularVelocity());
    }
    if (const auto *dynamic = a.is<PxRigidDynamic>())
      scalar("IsSleeping", 0, "value", dynamic->isSleeping());
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
  std::array<PxScene *, 2> scenes{};
  std::vector<PxRigidActor *> rigids;
  std::vector<PxJoint *> joints;
  std::array<Articulation, 2> articulations;
  PxClothFabric *fabric = nullptr;
  std::array<PxCloth *, 2> cloths{};
  PxParticleSystem *particles = nullptr;
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
    dispatcher = PxDefaultCpuDispatcherCreate(0);
    need(dispatcher, "dispatcher");
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    for (auto &scene : scenes) {
      scene = physics->createScene(desc);
      need(scene, "scene");
      scene->getScenePvdClient()->setScenePvdFlag(
          PxPvdSceneFlag::eTRANSMIT_SCENEQUERIES, true);
    }
    material = physics->createMaterial(.4f, .3f, .2f);
    need(material, "material");
  }
  void shape(PxRigidActor &actor) {
    auto *s =
        physics->createShape(PxBoxGeometry(.5f, .5f, .5f), *material, true);
    need(s, "shape");
    actor.attachShape(*s);
    s->release();
  }
  PxRigidActor *rigid(PxScene &scene, PxVec3 p, bool dynamic,
                      const char *name) {
    auto *a = dynamic ? static_cast<PxRigidActor *>(
                            physics->createRigidDynamic(PxTransform(p)))
                      : static_cast<PxRigidActor *>(
                            physics->createRigidStatic(PxTransform(p)));
    need(a, "rigid");
    a->setName(name);
    shape(*a);
    rigids.push_back(a);
    scene.addActor(*a);
    return a;
  }
  void articulation(Articulation &a, PxVec3 p, bool sleep) {
    a.value = physics->createArticulation();
    need(a.value, "articulation");
    a.value->setName(sleep ? "sleep_articulation" : "awake_articulation");
    for (unsigned i = 0; i < 2; ++i) {
      auto *link = a.value->createLink(i ? a.links[0] : nullptr,
                                       PxTransform(p + PxVec3(0, float(i), 0)));
      need(link, "link");
      link->setName(sleep ? (i ? "sleep_link1" : "sleep_link0")
                          : (i ? "awake_link1" : "awake_link0"));
      shape(*link);
      a.links.push_back(link);
      if (auto *j = link->getInboundJoint()) {
        j->setParentPose(PxTransform(PxVec3(0, .5f, 0)));
        j->setChildPose(PxTransform(PxVec3(0, -.5f, 0)));
      }
    }
    scenes[0]->addArticulation(*a.value);
    if (sleep)
      a.value->putToSleep();
    else
      a.value->setWakeCounter(100);
  }
  void cloth() {
    const std::array<PxClothParticle, 4> points{{{PxVec3(0, 0, 0), 0},
                                                 {PxVec3(1, 0, 0), 0},
                                                 {PxVec3(1, 0, 1), 1},
                                                 {PxVec3(0, 0, 1), 1}}};
    const std::array<PxU32, 4> quad{0, 1, 2, 3};
    PxClothMeshDesc desc;
    desc.points.count = static_cast<PxU32>(points.size());
    desc.points.stride = sizeof(PxClothParticle);
    desc.points.data = &points[0].pos;
    desc.invMasses.count = static_cast<PxU32>(points.size());
    desc.invMasses.stride = sizeof(PxClothParticle);
    desc.invMasses.data = &points[0].invWeight;
    desc.quads.count = 1;
    desc.quads.stride = sizeof(quad);
    desc.quads.data = quad.data();
    need(desc.isValid(), "cloth descriptor");
    fabric = PxClothFabricCreate(*physics, desc, PxVec3(0, -1, 0));
    need(fabric, "fabric");
    for (unsigned i = 0; i < cloths.size(); ++i) {
      cloths[i] =
          physics->createCloth(PxTransform(PxVec3(110 + float(i) * 3, 30, 5)),
                               *fabric, points.data(), PxClothFlags());
      need(cloths[i], "cloth");
      cloths[i]->setName(i ? "sleep_cloth" : "awake_cloth");
      scenes[0]->addActor(*cloths[i]);
      if (i)
        cloths[i]->putToSleep();
      else
        cloths[i]->setWakeCounter(100);
    }
  }
  void particle() {
    particles = physics->createParticleSystem(16);
    need(particles, "particles");
    particles->setName("particles");
    particles->setParticleReadDataFlag(PxParticleReadDataFlag::ePOSITION_BUFFER,
                                       true);
    particles->setParticleReadDataFlag(PxParticleReadDataFlag::eFLAGS_BUFFER,
                                       true);
    const PxU32 index = 3;
    const PxVec3 position(120, 25, 8), velocity(0);
    PxParticleCreationData data;
    data.numParticles = 1;
    data.indexBuffer = PxStrideIterator<const PxU32>(&index);
    data.positionBuffer = PxStrideIterator<const PxVec3>(&position);
    data.velocityBuffer = PxStrideIterator<const PxVec3>(&velocity);
    need(particles->createParticles(data), "particle creation");
    scenes[0]->addActor(*particles);
  }
  ~Context() {
    for (auto *j : joints)
      j->release();
    for (auto *c : cloths)
      if (c)
        c->release();
    if (fabric)
      fabric->release();
    if (particles)
      particles->release();
    for (auto &a : articulations)
      if (a.value)
        a.value->release();
    for (auto *a : rigids)
      a->release();
    if (material)
      material->release();
    for (auto *s : scenes)
      if (s)
        s->release();
    if (dispatcher)
      dispatcher->release();
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
void record(Context &c, Csv &csv, const std::string &phase) {
  csv.phase = phase;
  for (auto *a : c.rigids) {
    csv.scene = id(a->getScene());
    csv.actor(*a);
  }
  csv.scene = id(c.scenes[0]);
  for (const auto &a : c.articulations) {
    for (auto *link : a.links)
      csv.actor(*link);
    csv.object = id(a.value);
    csv.name = a.value->getName();
    csv.kind = "PxArticulation";
    csv.scalar("IsSleeping", 0, "value", a.value->isSleeping());
  }
  for (auto *j : c.joints) {
    csv.object = id(j);
    csv.name = j->getName();
    csv.kind = j->getConcreteTypeName();
    csv.pose("LocalPose[0]", j->getLocalPose(PxJointActorIndex::eACTOR0));
    csv.pose("LocalPose[1]", j->getLocalPose(PxJointActorIndex::eACTOR1));
  }
  for (auto *cloth : c.cloths) {
    csv.object = id(cloth);
    csv.name = cloth->getName();
    csv.kind = "PxCloth";
    csv.pose("GlobalPose", cloth->getGlobalPose());
    csv.scalar("IsSleeping", 0, "value", cloth->isSleeping());
    auto *data = cloth->lockParticleData(PxDataAccessFlag::eREADABLE);
    need(data, "cloth read");
    for (PxU32 i = 0; i < cloth->getNbParticles(); ++i) {
      csv.vec("ParticleBuffer", data->particles[i].pos, i);
      csv.vec("@WorldParticle",
              cloth->getGlobalPose().transform(data->particles[i].pos), i);
    }
    data->unlock();
  }
  csv.object = id(c.particles);
  csv.name = "particles";
  csv.kind = "PxParticleSystem";
  auto *data = c.particles->lockParticleReadData();
  need(data, "particle read");
  csv.vec("Positions", data->positionBuffer[3], 3);
  data->unlock();
  PxVec3 normal;
  PxReal distance;
  c.particles->getProjectionPlane(normal, distance);
  csv.vec("ProjectionPlane.normal", normal);
  csv.scalar("ProjectionPlane.distance", 0, "value", distance);
}
void query(Context &c, Csv &csv, const std::string &phase, PxVec3 offset) {
  csv.phase = phase;
  csv.scene = id(c.scenes[0]);
  csv.object = 0;
  csv.name = "query_input";
  csv.kind = "raycast";
  const PxVec3 origin = PxVec3(100, 5, -10) - offset;
  PxRaycastBuffer hit;
  const bool blocked = c.scenes[0]->raycast(origin, PxVec3(0, 0, 1), 30, hit);
  csv.vec("Origin", origin);
  csv.scalar("@HasBlock", 0, "value", blocked);
  if (hit.hasBlock)
    csv.vec("@HitPosition", hit.block.position);
  PxSweepBuffer sweep;
  c.scenes[0]->sweep(PxSphereGeometry(.2f), PxTransform(origin),
                     PxVec3(0, 0, 1), 30, sweep);
  std::array<PxOverlapHit, 8> overlap_hits;
  PxOverlapBuffer overlap(overlap_hits.data(),
                          static_cast<PxU32>(overlap_hits.size()));
  c.scenes[0]->overlap(PxSphereGeometry(1),
                       PxTransform(PxVec3(100, 5, 0) - offset), overlap);
}
void run(const std::filesystem::path &prefix) {
  if (prefix.has_parent_path())
    std::filesystem::create_directories(prefix.parent_path());
  Context c;
  c.init(prefix.string() + "_capture.pxd2");
  auto *stationary = c.rigid(*c.scenes[0], {100, 5, 0}, false, "static");
  auto *awake = static_cast<PxRigidDynamic *>(
      c.rigid(*c.scenes[0], {102, 10, 0}, true, "awake"));
  awake->setWakeCounter(100);
  awake->setLinearVelocity(PxVec3(.1f, 0, 0));
  auto *sleeper = static_cast<PxRigidDynamic *>(
      c.rigid(*c.scenes[0], {104, 15, 0}, true, "sleeper"));
  sleeper->putToSleep();
  c.rigid(*c.scenes[1], {100, 40, 0}, false, "other_scene_static");
  c.articulation(c.articulations[0], {106, 20, 0}, false);
  c.articulation(c.articulations[1], {108, 25, 0}, true);
  c.cloth();
  c.particle();
  auto *j0 =
      PxFixedJointCreate(*c.physics, nullptr, PxTransform(PxVec3(102, 10, 0)),
                         awake, PxTransform(PxIdentity));
  auto *j1 = PxFixedJointCreate(*c.physics, sleeper, PxTransform(PxIdentity),
                                nullptr, PxTransform(PxVec3(104, 15, 0)));
  need(j0 && j1, "world joints");
  j0->setName("world_joint0");
  j1->setName("world_joint1");
  c.joints = {j0, j1};
  Csv csv(prefix.string() + "_expected.csv");
  auto *stream = pvdsdk::PvdDataStream::create(c.pvd);
  need(stream, "markers");
  struct Guard {
    pvdsdk::PvdDataStream *p;
    ~Guard() { p->release(); }
  } guard{stream};
  auto sample = [&](const std::string &phase) {
    record(c, csv, phase);
    stream->beginSection(c.scenes[0], phase.c_str());
    stream->endSection(c.scenes[0], phase.c_str());
    stream->beginSection(c.scenes[0], "validation_origin_sample");
    stream->endSection(c.scenes[0], "validation_origin_sample");
  };
  auto simulate = [&] {
    for (auto *s : c.scenes) {
      s->simulate(1.f / 60);
      need(s->fetchResults(true), "fetch");
    }
  };
  c.scenes[0]->getScenePvdClient()->updateCamera(
      "recorded_scene0", PxVec3(100, 10, -20), PxVec3(0, 1, 0),
      PxVec3(100, 10, 0));
  c.scenes[1]->getScenePvdClient()->updateCamera(
      "recorded_scene1", PxVec3(100, 40, -20), PxVec3(0, 0, 1),
      PxVec3(100, 40, 0));
  simulate(); // Establish the SDK's stable-sleep tracking and initial arrays.
  sample("origin_baseline");
  query(c, csv, "query_before_shift1", PxVec3(0));
  const PxVec3 shift1(100, 2, -3);
  c.scenes[0]->shiftOrigin(shift1);
  sample("origin_shift1_immediate");
  query(c, csv, "query_after_shift1", shift1);
  simulate();
  sample("origin_shift1_post");
  stationary->setGlobalPose(PxTransform(PxVec3(7, 8, 9)));
  c.cloths[0]->setGlobalPose(PxTransform(PxVec3(15, 16, 17)));
  c.scenes[0]->getScenePvdClient()->updateCamera(
      "recorded_scene0", PxVec3(0, 8, -17), PxVec3(0, 1, 0), PxVec3(0, 8, 3));
  sample("origin_explicit_refresh");
  const PxVec3 shift2(-11, 3, 4);
  c.scenes[0]->shiftOrigin(shift2);
  sample("origin_shift2_immediate");
  simulate();
  sample("origin_shift2_post");
  c.rigid(*c.scenes[0], {1000000.f, 50, 25}, false, "rounding_static");
  for (unsigned i = 0; i < 64; ++i)
    c.scenes[0]->shiftOrigin(PxVec3(.01f, -.005f, .0025f));
  sample("origin_rounding_post");
  query(c, csv, "query_after_small_shifts", shift1 + shift2 +
              PxVec3(.01f, -.005f, .0025f) * 64.f);
  simulate();
  sample("origin_rounding_flush");
  need(bool(csv.out), "oracle write");
  need(c.errors.failures == 0, "SDK errors");
  std::cout << "origin-shift fixture: " << csv.fields
            << " getter/input fields, 66 shifts, 2 scenes, 8 markers\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    need(argc == 2, "usage: origin_shift_capture output_prefix");
    Assertions assertions;
    physx::PxSetAssertHandler(assertions);
    run(std::filesystem::u8path(argv[1]));
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
