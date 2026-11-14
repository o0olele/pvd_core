#include <PxPhysicsAPI.h>
#include <cstdint>
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
struct Errors : PxErrorCallback {
  unsigned failures = 0;
  void reportError(PxErrorCode::Enum code, const char *message, const char *,
                   int) override {
    std::cerr << "SDK [" << unsigned(code) << "] " << message << '\n';
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
  std::vector<PxRigidActor *> actors;
  void init(const std::string &path) {
    foundation = PxCreateFoundation(PX_FOUNDATION_VERSION, allocator, errors);
    need(foundation, "foundation");
    pvd = PxCreatePvd(*foundation);
    need(pvd, "PVD");
    transport = PxDefaultPvdFileTransportCreate(path.c_str());
    need(transport, "file transport");
    need(pvd->connect(*transport, PxPvdInstrumentationFlag::eDEBUG), "connect");
    physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation,
                              PxTolerancesScale(), true, pvd);
    need(physics, "physics");
    dispatcher = PxDefaultCpuDispatcherCreate(0);
    need(dispatcher, "dispatcher");
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0, -9.81f, 0);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    scene = physics->createScene(desc);
    need(scene, "scene");
    material = physics->createMaterial(.4f, .3f, .2f);
    need(material, "material");
  }
  ~Context() {
    for (auto *a : actors)
      a->release();
    if (material)
      material->release();
    if (scene)
      scene->release();
    if (dispatcher)
      dispatcher->release();
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
unsigned number(const char *s, unsigned lower, unsigned upper) {
  std::size_t used = 0;
  auto value = std::stoul(s, &used);
  need(used == std::string(s).size() && value >= lower && value <= upper,
       "argument outside supported range");
  return static_cast<unsigned>(value);
}
void run(const std::filesystem::path &prefix, unsigned count, unsigned frames) {
  if (!prefix.parent_path().empty())
    std::filesystem::create_directories(prefix.parent_path());
  Context c;
  c.init(prefix.string() + "_capture.pxd2");
  std::vector<std::string> names;
  names.reserve(count);
  c.actors.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    names.push_back("stress_actor_" + std::to_string(i));
    PxTransform pose(
        PxVec3(float(i % 32) * 2, 10 + float(i % 5) * .1f, float(i / 32) * 2),
        PxQuat(.13f * float(i % 7), PxVec3(0, 1, 0)));
    PxRigidActor *actor =
        i % 4 ? static_cast<PxRigidActor *>(c.physics->createRigidDynamic(pose))
              : static_cast<PxRigidActor *>(c.physics->createRigidStatic(pose));
    need(actor, "actor");
    c.actors.push_back(actor);
    actor->setName(names.back().c_str());
    PxShape *shape = nullptr;
    if (i % 3 == 0)
      shape = c.physics->createShape(PxBoxGeometry(.3f, .4f, .5f), *c.material,
                                     true);
    else if (i % 3 == 1)
      shape = c.physics->createShape(PxSphereGeometry(.3f), *c.material, true);
    else
      shape = c.physics->createShape(PxCapsuleGeometry(.2f, .35f), *c.material,
                                     true);
    need(shape, "shape");
    actor->attachShape(*shape);
    shape->release();
    c.scene->addActor(*actor);
  }
  std::ofstream csv(prefix.string() + "_expected.csv", std::ios::binary);
  need(bool(csv), "oracle CSV");
  csv << std::setprecision(std::numeric_limits<float>::max_digits10)
      << "frame,actor_id,dynamic,x,y,z,qx,qy,qz,qw\n";
  for (unsigned frame = 0; frame < frames; ++frame) {
    // Native EndFrame precedes fetch's dynamic pose update. Record the actual
    // SDK state before simulation, never infer a pose from a frame offset.
    for (auto *actor : c.actors) {
      auto p = actor->getGlobalPose();
      csv << frame << ','
          << static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(actor))
          << ',' << unsigned(actor->is<PxRigidDynamic>() != nullptr) << ','
          << p.p.x << ',' << p.p.y << ',' << p.p.z << ',' << p.q.x << ','
          << p.q.y << ',' << p.q.z << ',' << p.q.w << '\n';
    }
    c.scene->simulate(1.f / 60);
    need(c.scene->fetchResults(true), "fetch");
  }
  csv.flush();
  need(bool(csv), "flush oracle");
  need(!c.errors.failures, "SDK errors");
  std::cout << "SDK stress capture: " << count << " actors / " << frames
            << " native frames\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    need(argc >= 2 && argc <= 4, "usage: pvd_physx34_stress_capture prefix "
                                 "[actors=128:16..2048] [frames=48:16..1024]");
    run(argv[1], argc > 2 ? number(argv[2], 16, 2048) : 128,
        argc > 3 ? number(argv[3], 16, 1024) : 48);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
