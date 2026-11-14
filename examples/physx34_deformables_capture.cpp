#include <PxPhysicsAPI.h>
#include <PxPvdDataStream.h>
#include <array>
#include <atomic>
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
namespace wire = physx::pvdsdk;
void need(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::uint64_t id(const void *value) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}
struct Assertions : PxAssertHandler {
  void operator()(const char *e, const char *f, int line, bool &) override {
    std::fprintf(stderr, "PhysX assertion: %s (%s:%d)\n", e, f, line);
    std::fflush(nullptr);
    std::_Exit(3);
  }
};
struct Errors : PxErrorCallback {
  unsigned failures = 0;
  void reportError(PxErrorCode::Enum code, const char *message,
                   const char *file, int line) override {
    std::fprintf(stderr, "PhysX [%u]: %s (%s:%d)\n", unsigned(code), message,
                 file, line);
    if (code != PxErrorCode::eDEBUG_INFO && code != PxErrorCode::eDEBUG_WARNING)
      ++failures;
  }
};
struct Context {
  PxDefaultAllocator allocator;
  Errors errors;
  PxFoundation *foundation = nullptr;
  PxPvd *debugger = nullptr;
  PxPvdTransport *transport = nullptr;
  PxPhysics *physics = nullptr;
  PxDefaultCpuDispatcher *dispatcher = nullptr;
  PxScene *scene = nullptr;
  std::array<PxParticleBase *, 4> actors{};
  PxClothFabric *fabric = nullptr;
  PxCloth *cloth = nullptr;
  void init(const std::string &path) {
    foundation = PxCreateFoundation(PX_FOUNDATION_VERSION, allocator, errors);
    need(foundation, "foundation");
    debugger = PxCreatePvd(*foundation);
    need(debugger, "PVD");
    transport = PxDefaultPvdFileTransportCreate(path.c_str());
    need(transport, "file transport");
    need(debugger->connect(*transport, PxPvdInstrumentationFlag::eDEBUG),
         "PVD debug connect");
    physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation,
                              PxTolerancesScale(), true, debugger);
    need(physics, "physics");
    dispatcher = PxDefaultCpuDispatcherCreate(0);
    need(dispatcher, "dispatcher");
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0, -9.81f, 0);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    scene = physics->createScene(desc);
    need(scene, "scene");
  }
  ~Context() {
    if (cloth)
      cloth->release();
    if (fabric)
      fabric->release();
    for (auto *p : actors)
      if (p)
        p->release();
    if (scene)
      scene->release();
    if (dispatcher)
      dispatcher->release();
    if (physics)
      physics->release();
    if (debugger) {
      debugger->disconnect();
      debugger->release();
    }
    if (transport)
      transport->release();
    if (foundation)
      foundation->release();
  }
};
void flags(PxParticleBase &actor, bool enabled) {
  actor.setParticleReadDataFlag(PxParticleReadDataFlag::ePOSITION_BUFFER,
                                enabled);
  actor.setParticleReadDataFlag(PxParticleReadDataFlag::eVELOCITY_BUFFER,
                                enabled);
  actor.setParticleReadDataFlag(PxParticleReadDataFlag::eFLAGS_BUFFER, enabled);
}
void create(PxParticleBase &actor, const std::vector<PxU32> &indices, float x,
            float y) {
  std::vector<PxVec3> positions, velocities;
  for (std::size_t i = 0; i < indices.size(); ++i) {
    positions.emplace_back(x + float(i) * .3f, y + float(i) * .2f,
                           float(i) * .15f);
    velocities.emplace_back(.2f + float(i) * .1f, .3f, float(i) * -.1f);
  }
  PxParticleCreationData data;
  data.numParticles = static_cast<PxU32>(indices.size());
  data.indexBuffer = PxStrideIterator<const PxU32>(indices.data());
  data.positionBuffer = PxStrideIterator<const PxVec3>(positions.data());
  data.velocityBuffer = PxStrideIterator<const PxVec3>(velocities.data());
  need(actor.createParticles(data), "create sparse particles");
}
PxParticleFluid *fluid(Context &context, const char *name,
                       const std::vector<PxU32> &indices) {
  auto *actor = context.physics->createParticleFluid(128);
  need(actor, "particle fluid");
  actor->setName(name);
  actor->setRestParticleDistance(.1f);
  flags(*actor, true);
  actor->setParticleReadDataFlag(PxParticleReadDataFlag::eDENSITY_BUFFER, true);
  create(*actor, indices, 10, 7);
  context.scene->addActor(*actor);
  return actor;
}
void vector(std::ofstream &out, const PxVec3 &value) {
  out << ',' << value.x << ',' << value.y << ',' << value.z;
}
std::vector<PxClothParticle> cloth_particles() {
  std::vector<PxClothParticle> result;
  for (unsigned row = 0; row < 4; ++row)
    for (unsigned column = 0; column < 4; ++column)
      result.push_back({PxVec3(float(column) * .4f, 0, float(row) * .4f),
                        row == 0 && (column == 0 || column == 3) ? 0.f : 1.f});
  return result;
}
void create_cloth(Context &context, const char *name) {
  auto particles = cloth_particles();
  if (!context.fabric) {
    std::vector<PxU32> quads;
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned column = 0; column < 3; ++column) {
        const auto index = row * 4 + column;
        for (const auto vertex : {index, index + 1, index + 5, index + 4})
          quads.push_back(vertex);
      }
    PxClothMeshDesc desc;
    desc.points.data = &particles[0].pos;
    desc.points.count = static_cast<PxU32>(particles.size());
    desc.points.stride = sizeof(PxClothParticle);
    desc.invMasses.data = &particles[0].invWeight;
    desc.invMasses.count = desc.points.count;
    desc.invMasses.stride = sizeof(PxClothParticle);
    desc.quads.data = quads.data();
    desc.quads.count = static_cast<PxU32>(quads.size() / 4);
    desc.quads.stride = 4 * sizeof(PxU32);
    need(desc.isValid(), "cloth mesh descriptor");
    context.fabric =
        PxClothFabricCreate(*context.physics, desc, PxVec3(0, -1, 0));
    need(context.fabric, "CPU cloth fabric cooking");
  }
  context.cloth = context.physics->createCloth(
      PxTransform(PxVec3(-4, 6, 2),
                  PxQuat(.35f, PxVec3(0, 1, 0)) * PxQuat(.2f, PxVec3(1, 0, 0))),
      *context.fabric, particles.data(), PxClothFlags());
  need(context.cloth, "CPU cloth");
  context.cloth->setName(name);
  context.cloth->setSolverFrequency(240);
  context.scene->addActor(*context.cloth);
}
void record_cloth(std::ofstream &out, Context &context, unsigned step) {
  const auto &cloth = *context.cloth;
  const auto pose = cloth.getGlobalPose();
  out << "cloth_owner," << step << ',' << id(context.scene) << ',' << id(&cloth)
      << ',' << id(context.fabric) << ',' << cloth.getNbParticles();
  vector(out, pose.p);
  out << ',' << pose.q.x << ',' << pose.q.y << ',' << pose.q.z << ','
      << pose.q.w << ',' << cloth.isSleeping() << ','
      << unsigned(PxU16(cloth.getClothFlags())) << ','
      << unsigned(PxU8(cloth.getActorFlags())) << ',' << (step != 0) << ','
      << (cloth.getName() ? cloth.getName() : "") << '\n';
  auto *data = context.cloth->lockParticleData(PxDataAccessFlag::eREADABLE);
  need(data, "cloth particle lock");
  for (PxU32 i = 0; i < cloth.getNbParticles(); ++i) {
    const auto &particle = data->particles[i];
    out << "cloth_vertex," << step << ',' << id(&cloth) << ',' << i;
    vector(out, particle.pos);
    vector(out, pose.transform(particle.pos));
    out << ',' << particle.invWeight << '\n';
  }
  data->unlock();
  auto &fabric = *context.fabric;
  std::vector<PxU32> indices(fabric.getNbParticleIndices()),
      sets(fabric.getNbSets());
  std::vector<PxReal> rest(fabric.getNbRestvalues());
  std::vector<PxClothFabricPhase> phases(fabric.getNbPhases());
  need(fabric.getParticleIndices(indices.data(),
                                 static_cast<PxU32>(indices.size())) ==
           indices.size(),
       "fabric indices");
  need(fabric.getSets(sets.data(), static_cast<PxU32>(sets.size())) ==
           sets.size(),
       "fabric sets");
  need(fabric.getRestvalues(rest.data(), static_cast<PxU32>(rest.size())) ==
           rest.size(),
       "fabric rest values");
  need(fabric.getPhases(phases.data(), static_cast<PxU32>(phases.size())) ==
           phases.size(),
       "fabric phases");
  for (std::size_t i = 0, set = 0; i < rest.size(); ++i) {
    while (set < sets.size() && i >= sets[set])
      ++set;
    need(set < sets.size(), "fabric set coverage");
    out << "cloth_constraint," << step << ',' << id(&cloth) << ',' << i << ','
        << indices[i * 2] << ',' << indices[i * 2 + 1] << ',' << rest[i] << ','
        << set << '\n';
  }
  for (std::size_t i = 0; i < phases.size(); ++i)
    out << "cloth_phase," << step << ',' << id(&cloth) << ',' << i << ','
        << unsigned(phases[i].phaseType) << ',' << phases[i].setIndex << '\n';
}
void record(std::ofstream &out, Context &context, unsigned step,
            PxParticleBase &actor, const char *kind,
            PxParticleBase *twin = nullptr) {
  const auto original = actor.getParticleReadDataFlags();
  // The CPU SDK internally keeps full velocities even with read flags off.
  // Read flags are immutable in-scene. A same-parameter, same-initial-velocity
  // SDK twin supplies the disabled velocity oracle without modifying this
  // actor.
  PxParticleReadData *data =
      actor.is<PxParticleFluid>()
          ? static_cast<PxParticleReadData *>(
                actor.is<PxParticleFluid>()->lockParticleFluidReadData())
          : actor.lockParticleReadData();
  need(data, "read data");
  PxParticleReadData *twin_data = twin ? twin->lockParticleReadData() : nullptr;
  out << "owner," << step << ',' << kind << ',' << id(context.scene) << ','
      << id(&actor) << ',' << unsigned(PxU16(original)) << ','
      << unsigned(PxU16(actor.getParticleBaseFlags())) << ','
      << actor.getMaxParticles() << ',' << data->nbValidParticles << ','
      << data->validParticleRange << ','
      << (actor.getName() ? actor.getName() : "") << '\n';
  for (PxU32 index = 0; index < data->validParticleRange; ++index) {
    if (!(data->validParticleBitmap[index >> 5] & (1u << (index & 31))))
      continue;
    out << "particle," << step << ',' << id(&actor) << ',' << index;
    vector(out, data->positionBuffer[index]);
    if (data->velocityBuffer.ptr())
      vector(out, data->velocityBuffer[index]);
    else {
      need(twin_data && twin_data->velocityBuffer.ptr() &&
               (twin_data->validParticleBitmap[index >> 5] &
                (1u << (index & 31))),
           "valid velocity twin");
      vector(out, twin_data->velocityBuffer[index]);
    }
    out << ',' << unsigned(PxU16(data->flagsBuffer[index]));
    if (data->restOffsetBuffer.ptr())
      out << ',' << data->restOffsetBuffer[index];
    else
      out << ',';
    if (auto *f = actor.is<PxParticleFluid>()) {
      auto *rd = static_cast<PxParticleFluidReadData *>(data);
      if (rd->densityBuffer.ptr())
        out << ',' << rd->densityBuffer[index];
      else
        out << ',';
    } else
      out << ',';
    if (data->collisionNormalBuffer.ptr())
      vector(out, data->collisionNormalBuffer[index]);
    else
      out << ",,,";
    out << '\n';
  }
  data->unlock();
  if (twin_data)
    twin_data->unlock();
  need(bool(out), "oracle output");
}
void run(const std::filesystem::path &prefix) {
  if (!prefix.parent_path().empty())
    std::filesystem::create_directories(prefix.parent_path());
  Context context;
  context.init(prefix.string() + "_capture.pxd2");
  std::ofstream out(prefix.string() + "_expected.csv");
  need(bool(out), "CSV output");
  out << std::setprecision(9);
  out << "# "
         "owner,step,kind,scene,id,read_flags,base_flags,max_particles,count,"
         "valid_range,name\n# "
         "particle,step,owner,index,position(3),velocity(3),flags,rest_offset,"
         "density,normal(3)\n# "
         "cloth_owner,step,scene,id,fabric,count,position(3),quaternion(4),"
         "sleeping,cloth_flags,actor_flags,positions_captured,name\n# "
         "cloth_vertex,step,owner,index,local(3),world(3),inverse_mass\n# "
         "cloth_constraint,step,owner,index,particle0,particle1,rest_length,"
         "set_index\n# cloth_phase,step,owner,index,phase_type,set_index\n";
  create_cloth(context, "Native cloth - pinned corners");
  auto *system = context.physics->createParticleSystem(128);
  need(system, "particle system");
  context.actors[0] = system;
  system->setName("Sparse system");
  flags(*system, true);
  create(*system, {0, 7, 31, 32, 70}, 0, 5);
  context.scene->addActor(*system);
  context.actors[1] = fluid(context, "Sparse fluid", {2, 9, 40});
  auto *read_off = context.physics->createParticleSystem(128);
  need(read_off, "read-off system");
  context.actors[2] = read_off;
  read_off->setName("CPU read_flags off");
  flags(*read_off, true);
  read_off->setParticleReadDataFlag(PxParticleReadDataFlag::eVELOCITY_BUFFER,
                                    false);
  create(*read_off, {0, 31}, 20, 9);
  context.scene->addActor(*read_off);
  auto *twin = context.physics->createParticleSystem(128);
  need(twin, "read-on twin");
  context.actors[3] = twin;
  twin->setName("Read-on oracle twin");
  flags(*twin, true);
  create(*twin, {0, 31}, 40, 9);
  context.scene->addActor(*twin);
  auto *stream = wire::PvdDataStream::create(context.debugger);
  need(stream, "validation stream");
  struct Guard {
    wire::PvdDataStream *value;
    ~Guard() { value->release(); }
  } guard{stream};
  auto sample = [&](unsigned step, const char *section) {
    record(out, context, step, *context.actors[0], "system");
    record(out, context, step, *context.actors[1], "fluid");
    record(out, context, step, *context.actors[2], "read_off", twin);
    record(out, context, step, *context.actors[3], "twin");
    record_cloth(out, context, step);
    stream->beginSection(context.scene, section);
    stream->endSection(context.scene, section);
  };
  sample(0, "initial_deformables");
  for (unsigned step = 1; step <= 8; ++step) {
    if (step == 2) {
      PxU32 index = 7;
      system->releaseParticles(1, PxStrideIterator<const PxU32>(&index));
      index = 9;
      context.actors[1]->releaseParticles(
          1, PxStrideIterator<const PxU32>(&index));
    }
    if (step == 3) {
      create(*system, {7}, 2, 8);
      create(*context.actors[1], {9}, 12, 10);
    }
    if (step == 4)
      system->releaseParticles();
    if (step == 5)
      create(*system, {1, 32, 70}, 3, 11);
    if (step == 6) {
      read_off->setParticleBaseFlag(PxParticleBaseFlag::eENABLED, false);
      twin->setParticleBaseFlag(PxParticleBaseFlag::eENABLED, false);
    }
    if (step == 7) {
      read_off->setParticleBaseFlag(PxParticleBaseFlag::eENABLED, true);
      twin->setParticleBaseFlag(PxParticleBaseFlag::eENABLED, true);
      context.actors[1]->release();
      context.actors[1] = nullptr;
      context.actors[1] = fluid(context, "Recreated fluid", {3, 65});
      context.cloth->release();
      context.cloth = nullptr;
      create_cloth(context, "Recreated cloth - same fabric");
    }
    if (step == 2) {
      auto *data = context.cloth->lockParticleData(PxDataAccessFlag::eREADABLE);
      need(data, "cloth pin edit read");
      std::vector<PxClothParticle> edited(
          data->particles, data->particles + context.cloth->getNbParticles());
      data->unlock();
      edited[6].invWeight = 0;
      context.cloth->setParticles(edited.data(), nullptr);
    }
    if (step == 4)
      context.cloth->putToSleep();
    else {
      if (step == 5)
        context.cloth->wakeUp();
      context.cloth->setTargetPose(PxTransform(
          PxVec3(-4 + float(step) * .08f, 6, 2 - float(step) * .03f),
          PxQuat(.35f + float(step) * .025f, PxVec3(0, 1, 0)) *
              PxQuat(.2f, PxVec3(1, 0, 0))));
    }
    context.scene->simulate(1.f / 60);
    need(context.scene->fetchResults(true), "fetch");
    sample(step, "validation_after_fetch");
  }
  need(!context.errors.failures, "SDK errors");
  std::cout << "Native cloth/particle oracle generated at " << prefix.string()
            << "_{capture.pxd2,expected.csv}\n";
}
} // namespace
int main(int argc, char **argv) {
  Assertions assertion;
  PxSetAssertHandler(assertion);
  try {
    need(argc == 2, "usage: pvd_physx34_deformables_capture output_prefix");
    run(argv[1]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
