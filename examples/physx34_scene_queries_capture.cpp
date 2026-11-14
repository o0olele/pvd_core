// Independent PhysX 3.4 scene-query oracle: expectations come from public
// query input objects and returned SDK buffers, never pvd_core decoding.
#include <PxPhysicsAPI.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <task/PxCpuDispatcher.h>
#include <task/PxTask.h>
#include <type_traits>
#include <vector>
namespace {
using namespace physx;
void need(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
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
  std::atomic<unsigned> failures{0};
  void reportError(PxErrorCode::Enum code, const char *e, const char *f,
                   int line) override {
    std::fprintf(stderr, "SDK %d: %s %s:%d\n", int(code), e, f, line);
    if (code != PxErrorCode::eDEBUG_INFO && code != PxErrorCode::eDEBUG_WARNING)
      ++failures;
  }
};
struct Dispatcher : PxCpuDispatcher {
  void submitTask(PxBaseTask &task) override {
    task.run();
    task.release();
  }
  PxU32 getWorkerCount() const override { return 0; }
};
PxFilterFlags simulation_filter(PxFilterObjectAttributes, PxFilterData,
                                PxFilterObjectAttributes, PxFilterData,
                                PxPairFlags &flags, const void *, PxU32) {
  flags = PxPairFlag::eCONTACT_DEFAULT;
  return PxFilterFlag::eDEFAULT;
}
struct Csv {
  std::ofstream file;
  unsigned frame = 0;
  std::uint64_t scene = 0;
  bool batch = false;
  const char *kind = "";
  unsigned sequence = 0;
  explicit Csv(const std::string &path) : file(path) {
    need(bool(file), "open oracle CSV");
    file << std::setprecision(9)
         << "frame,scene,batched,kind,sequence,field,item,type,value\n";
  }
  template <class T>
  void row(const std::string &key, unsigned item, char type, T value) {
    file << frame << ',' << scene << ',' << unsigned(batch) << ',' << kind
         << ',' << sequence << ',' << key << ',' << item << ',' << type << ','
         << value << '\n';
  }
  void u(const std::string &name, std::uint64_t value, unsigned item = 0) {
    row(name, item, 'u', value);
  }
  void f(const std::string &name, float value, unsigned item = 0) {
    row(name, item, 'f', value);
  }
  void v(const std::string &name, PxVec3 v, unsigned item = 0) {
    f(name + ".x", v.x, item);
    f(name + ".y", v.y, item);
    f(name + ".z", v.z, item);
  }
  void pose(const std::string &name, const PxTransform &p) {
    v(name + ".p", p.p);
    f(name + ".q.x", p.q.x);
    f(name + ".q.y", p.q.y);
    f(name + ".q.z", p.q.z);
    f(name + ".q.w", p.q.w);
  }
  void filter(const PxQueryFilterData &fd) {
    u("filter_flags", PxU32(fd.flags));
    u("filter_data", fd.data.word0, 0);
    u("filter_data", fd.data.word1, 1);
    u("filter_data", fd.data.word2, 2);
    u("filter_data", fd.data.word3, 3);
  }
  void geometry(const PxGeometry &g) {
    if (g.getType() == PxGeometryType::eBOX) {
      u("geometry.kind", 0);
      v("geometry.extents", static_cast<const PxBoxGeometry &>(g).halfExtents);
    } else if (g.getType() == PxGeometryType::eSPHERE) {
      u("geometry.kind", 1);
      f("geometry.radius", static_cast<const PxSphereGeometry &>(g).radius);
    } else if (g.getType() == PxGeometryType::eCAPSULE) {
      u("geometry.kind", 2);
      const auto &c = static_cast<const PxCapsuleGeometry &>(g);
      f("geometry.radius", c.radius);
      f("geometry.half_height", c.halfHeight);
    } else if (g.getType() == PxGeometryType::eCONVEXMESH) {
      u("geometry.kind", 3);
      const auto &c = static_cast<const PxConvexMeshGeometry &>(g);
      u("geometry.resource", id(c.convexMesh));
      v("geometry.scale", c.scale.scale);
      f("geometry.rotation.x", c.scale.rotation.x);
      f("geometry.rotation.y", c.scale.rotation.y);
      f("geometry.rotation.z", c.scale.rotation.z);
      f("geometry.rotation.w", c.scale.rotation.w);
      u("geometry.flags", PxU32(c.meshFlags));
    }
  }
  template <class Buffer> void result(const Buffer &r) {
    u("hit_count", r.getNbAnyHits());
    u("sdk_has_block", r.hasBlock);
    u("sdk_touches", r.nbTouches);
    for (PxU32 i = 0; i < r.getNbAnyHits(); ++i) {
      const auto &h = r.getAnyHit(i);
      using H = std::decay_t<decltype(h)>;
      u("hit.shape", id(h.shape), i);
      u("hit.actor", id(h.actor), i);
      u("hit.face", h.faceIndex, i);
      if constexpr (!std::is_same_v<H, PxOverlapHit>) {
        u("hit.flags", PxU32(h.flags), i);
        f("hit.distance", h.distance, i);
        if (h.flags & PxHitFlag::ePOSITION)
          v("hit.position", h.position, i);
        if (h.flags & PxHitFlag::eNORMAL)
          v("hit.normal", h.normal, i);
        if constexpr (std::is_same_v<H, PxRaycastHit>) {
          if (h.flags & PxHitFlag::eUV) {
            f("hit.u", h.u, i);
            f("hit.v", h.v, i);
          }
        }
      } else
        u("hit.flags", 0, i);
    }
  }
};
struct Context {
  PxDefaultAllocator allocator;
  Errors errors;
  Dispatcher dispatcher;
  PxFoundation *foundation = nullptr;
  PxPvd *pvd = nullptr;
  PxPvdTransport *transport = nullptr;
  PxPhysics *physics = nullptr;
  PxCooking *cooking = nullptr;
  PxMaterial *material = nullptr;
  PxConvexMesh *convex = nullptr;
  std::array<PxScene *, 2> scenes{};
  std::vector<PxRigidStatic *> actors;
  explicit Context(const std::string &path) {
    foundation = PxCreateFoundation(PX_FOUNDATION_VERSION, allocator, errors);
    need(foundation, "foundation");
    pvd = PxCreatePvd(*foundation);
    transport = PxDefaultPvdFileTransportCreate(path.c_str());
    need(pvd && transport &&
             pvd->connect(*transport, PxPvdInstrumentationFlag::eDEBUG),
         "PVD connect");
    physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation,
                              PxTolerancesScale(), true, pvd);
    need(physics, "physics");
    cooking = PxCreateCooking(PX_PHYSICS_VERSION, *foundation,
                              PxCookingParams(physics->getTolerancesScale()));
    need(cooking, "cooking");
    material = physics->createMaterial(.5f, .5f, 0);
    need(material, "material");
    const PxVec3 points[] = {{-.4f, -.4f, -.4f}, {.4f, -.4f, -.4f},
                             {-.4f, .4f, -.4f},  {.4f, .4f, -.4f},
                             {-.4f, -.4f, .4f},  {.4f, -.4f, .4f},
                             {-.4f, .4f, .4f},   {.4f, .4f, .4f}};
    PxConvexMeshDesc mesh;
    mesh.points.data = points;
    mesh.points.count = 8;
    mesh.points.stride = sizeof(PxVec3);
    mesh.flags = PxConvexFlag::eCOMPUTE_CONVEX;
    convex =
        cooking->createConvexMesh(mesh, physics->getPhysicsInsertionCallback());
    need(convex, "query convex mesh");
    for (unsigned s = 0; s < 2; ++s) {
      PxSceneDesc d(physics->getTolerancesScale());
      d.gravity = PxVec3(0);
      d.cpuDispatcher = &dispatcher;
      d.filterShader = simulation_filter;
      scenes[s] = physics->createScene(d);
      need(scenes[s], "scene");
      scenes[s]->getScenePvdClient()->setScenePvdFlag(
          PxPvdSceneFlag::eTRANSMIT_SCENEQUERIES, true);
      for (unsigned i = 0; i < 3; ++i) {
        auto *body = physics->createRigidStatic(
            PxTransform(PxVec3(float(s) * 20 + float(i) * 3, 0, 0)));
        auto *shape =
            physics->createShape(PxBoxGeometry(.5f, .5f, .5f), *material, true);
        need(body && shape, "query target");
        shape->setName(i == 0 ? "target_first" : "target_other");
        shape->setQueryFilterData(PxFilterData(~PxU32(0),~PxU32(0),~PxU32(0),~PxU32(0)));
        body->attachShape(*shape);
        shape->release();
        scenes[s]->addActor(*body);
        actors.push_back(body);
      }
    }
  }
  ~Context() {
    for (auto *p : actors)
      p->release();
    for (auto *p : scenes)
      if (p)
        p->release();
    if (convex)
      convex->release();
    if (material)
      material->release();
    if (cooking)
      cooking->release();
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
PxQueryFilterData fd(unsigned scene, bool touches) {
  PxQueryFilterData f;
  f.data = PxFilterData(scene, 42, 0x12345678u, 0xfedcba98u);
  f.flags = PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC;
  if (touches)
    f.flags |= PxQueryFlag::eNO_BLOCK;
  return f;
}
void inputs(Csv &csv, const PxQueryFilterData &filter, const PxVec3 &origin,
            const PxVec3 &direction, float distance) {
  csv.filter(filter);
  csv.v("origin", origin);
  csv.v("direction", direction);
  csv.f("distance", distance);
}
void native(Context &c, unsigned scene, Csv &csv) {
  auto &s = *c.scenes[scene];
  const PxVec3 origin(float(scene) * 20 - 3, 0, 0), direction(1, 0, 0);
  std::array<PxRaycastHit, 16> ray_touches{};
  std::array<PxSweepHit, 16> sweep_touches{};
  std::array<PxOverlapHit, 16> overlap_touches{};
  csv.batch = false;
  csv.kind = "raycast";
  for (unsigned i = 0; i < 4; ++i) {
    auto filter = fd(scene, i == 2);
    if (i == 3)
      filter.flags |= PxQueryFlag::eANY_HIT;
    const auto p = origin + PxVec3(0, i == 1 ? 10.f : 0.f, 0);
    PxRaycastBuffer result(i == 2 ? ray_touches.data() : nullptr,
                           i == 2 ? 16 : 0);
    s.raycast(p, direction, 12, result, PxHitFlag::eDEFAULT, filter);
    csv.sequence = i;
    inputs(csv, filter, p, direction, 12);
    csv.u("type", i == 3 ? 0 : (i == 2 ? 2 : 1));
    csv.result(result);
  }
  const PxSphereGeometry sphere(.25f);
  const PxBoxGeometry box(.25f, .3f, .35f);
  const PxCapsuleGeometry capsule(.2f, .4f);
  const PxConvexMeshGeometry convex(
      c.convex,
      PxMeshScale(PxVec3(.8f, 1.2f, .9f), PxQuat(.2f, PxVec3(0, 1, 0))));
  const PxGeometry *geometries[] = {&sphere, &box, &capsule, &convex};
  csv.kind = "sweep";
  for (unsigned i = 0; i < 4; ++i) {
    const auto filter = fd(scene, i == 2);
    const PxTransform pose(origin + PxVec3(0, i == 1 ? 10.f : 0.f, 0));
    PxSweepBuffer result(i == 2 ? sweep_touches.data() : nullptr,
                         i == 2 ? 16 : 0);
    s.sweep(*geometries[i], pose, direction, 12, result, PxHitFlag::eDEFAULT,
            filter);
    csv.sequence = i;
    csv.filter(filter);
    csv.v("direction", direction);
    csv.f("distance", 12);
    csv.pose("pose", pose);
    csv.geometry(*geometries[i]);
    csv.u("type", i == 1 ? 8 : (i == 3 ? 10 : 9));
    csv.result(result);
  }
  csv.kind = "overlap";
  for (unsigned i = 0; i < 4; ++i) {
    const auto filter = fd(scene, true);
    const PxTransform pose(
        PxVec3(float(scene) * 20 + 3, i == 1 ? 10.f : 0.f, 0));
    PxOverlapBuffer result(overlap_touches.data(), 16);
    s.overlap(*geometries[i], pose, result, filter);
    csv.sequence = i;
    csv.filter(filter);
    csv.pose("pose", pose);
    csv.geometry(*geometries[i]);
    csv.u("type", i == 0 ? 3 : (i == 1 ? 4 : (i == 2 ? 6 : 7)));
    csv.result(result);
  }
}
void batched(Context &c, unsigned scene, Csv &csv) {
  auto &s = *c.scenes[scene];
  const PxVec3 origin(float(scene) * 20 - 3, 0, 0), direction(1, 0, 0);
  const PxSphereGeometry geometry(.25f);
  std::array<PxRaycastQueryResult, 3> rays{};
  std::array<PxSweepQueryResult, 3> sweeps{};
  std::array<PxOverlapQueryResult, 2> overlaps{};
  std::array<PxRaycastHit, 48> ray_hits{};
  std::array<PxSweepHit, 48> sweep_hits{};
  std::array<PxOverlapHit, 32> overlap_hits{};
  PxBatchQueryDesc desc(3, 3, 2);
  desc.queryMemory.userRaycastResultBuffer = rays.data();
  desc.queryMemory.userSweepResultBuffer = sweeps.data();
  desc.queryMemory.userOverlapResultBuffer = overlaps.data();
  desc.queryMemory.userRaycastTouchBuffer = ray_hits.data();
  desc.queryMemory.raycastTouchBufferSize = 48;
  desc.queryMemory.userSweepTouchBuffer = sweep_hits.data();
  desc.queryMemory.sweepTouchBufferSize = 48;
  desc.queryMemory.userOverlapTouchBuffer = overlap_hits.data();
  desc.queryMemory.overlapTouchBufferSize = 32;
  auto *batch = s.createBatchQuery(desc);
  need(batch, "batch query create");
  for (unsigned i = 0; i < 3; ++i) {
    const auto filter = fd(scene, i == 2);
    const auto p = origin + PxVec3(0, i == 1 ? 10.f : 0.f, 0);
    batch->raycast(p, direction, 12, i == 2 ? 16 : 0, PxHitFlag::eDEFAULT,
                   filter);
    batch->sweep(geometry, PxTransform(p), direction, 12, i == 2 ? 16 : 0,
                 PxHitFlag::eDEFAULT, filter);
  }
  for (unsigned i = 0; i < 2; ++i)
    batch->overlap(
        PxSphereGeometry(4),
        PxTransform(PxVec3(float(scene) * 20 + 3, i ? 10.f : 0.f, 0)), 16,
        fd(scene, true));
  batch->execute();
  csv.batch = true;
  for (unsigned i = 0; i < 3; ++i) {
    need(rays[i].queryStatus == PxBatchQueryStatus::eSUCCESS &&
             sweeps[i].queryStatus == PxBatchQueryStatus::eSUCCESS,
         "batch success");
    const auto filter = fd(scene, i == 2);
    const auto p = origin + PxVec3(0, i == 1 ? 10.f : 0.f, 0);
    csv.sequence = i;
    csv.kind = "raycast";
    inputs(csv, filter, p, direction, 12);
    csv.u("type", i == 2 ? 2 : 1);
    csv.u("sdk_query_status", rays[i].queryStatus);
    csv.result(rays[i]);
    csv.kind = "sweep";
    csv.filter(filter);
    csv.v("direction", direction);
    csv.f("distance", 12);
    csv.pose("pose", PxTransform(p));
    csv.geometry(geometry);
    csv.u("type", 9);
    csv.u("sdk_query_status", sweeps[i].queryStatus);
    csv.result(sweeps[i]);
  }
  for (unsigned i = 0; i < 2; ++i) {
    need(overlaps[i].queryStatus == PxBatchQueryStatus::eSUCCESS,
         "batch overlap success");
    csv.kind = "overlap";
    csv.sequence = i;
    csv.filter(fd(scene, true));
    csv.pose("pose",
             PxTransform(PxVec3(float(scene) * 20 + 3, i ? 10.f : 0.f, 0)));
    csv.geometry(PxSphereGeometry(4));
    csv.u("type", 3);
    csv.u("sdk_query_status", overlaps[i].queryStatus);
    csv.result(overlaps[i]);
  }
  batch->release();
}
void run(const std::filesystem::path &prefix) {
  if (!prefix.parent_path().empty())
    std::filesystem::create_directories(prefix.parent_path());
  Context c(prefix.string() + "_capture.pxd2");
  Csv csv(prefix.string() + "_expected.csv");
  for (unsigned step = 0; step < 3; ++step)
    for (unsigned s = 0; s < 2; ++s) {
      csv.frame = step;
      csv.scene = id(c.scenes[s]);
      if (step < 2) {
        native(c, s, csv);
        batched(c, s, csv);
      }
      if (step == 2 && s == 1)
        c.scenes[s]->getScenePvdClient()->setScenePvdFlag(
            PxPvdSceneFlag::eTRANSMIT_SCENEQUERIES, false);
      csv.kind = "collection";
      csv.batch = false;
      csv.sequence = 0;
      csv.u("collected", !(step == 2 && s == 1));
      c.scenes[s]->simulate(1.f / 60);
      need(c.scenes[s]->fetchResults(true), "fetch");
    }
  csv.file.flush();
  need(bool(csv.file), "flush oracle");
  need(c.errors.failures == 0, "SDK errors");
  std::cout << "Scene queries: 2 scenes / 3 frames / native and batched "
               "raycast, sweep, overlap; collector-off and empty frames\n";
}
} // namespace
int main(int argc, char **argv) {
  Assertions assertions;
  PxSetAssertHandler(assertions);
  try {
    need(argc == 2, "usage: pvd_physx34_scene_queries_capture OUTPUT_PREFIX");
    run(argv[1]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
