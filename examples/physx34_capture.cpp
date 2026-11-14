// A real PhysX 3.4/PVD producer, deliberately independent of pvd_core.
// The .pxd2 output is PxDefaultPvdFileTransportCreate's raw communication stream,
// not the legacy viewer's saved/indexed container. Expected values come directly
// from getGlobalPose() before simulate() and after fetchResults(), never from the
// decoder under test. The SDK's "frame" end precedes its dynamic pose updates.
#include <PxPhysicsAPI.h>
#include <PxPvdDataStream.h>
#include <task/PxCpuDispatcher.h>
#include <task/PxTask.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace physx;
namespace pvd = physx::pvdsdk;
constexpr float timestep = 1.0f / 60.0f;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Errors final : PxErrorCallback {
    std::atomic<unsigned> failures{0};
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        std::fprintf(stderr, "PhysX [%d] %s (%s:%d)\n", int(code), message, file, line);
        if (code != PxErrorCode::eDEBUG_INFO && code != PxErrorCode::eDEBUG_WARNING)
            ++failures;
    }
};

struct Assertions final : PxAssertHandler {
    void operator()(const char* expression, const char* file, int line, bool&) override {
        std::fprintf(stderr, "PhysX assertion: %s (%s:%d)\n", expression, file, line);
        std::fflush(nullptr);
        std::_Exit(3); // A Debug SDK assertion must fail CTest rather than open a modal dialog.
    }
};

// PhysX permits a dispatcher with no workers: tasks execute synchronously on the
// calling thread. This keeps the fixture small and avoids the SDK's /MTd-built
// Extensions archive while the producer uses /MDd.
struct ImmediateDispatcher final : PxCpuDispatcher {
    void submitTask(PxBaseTask& task) override { task.run(); task.release(); }
    PxU32 getWorkerCount() const override { return 0; }
};

PxFilterFlags collisionFilter(PxFilterObjectAttributes, PxFilterData,
                              PxFilterObjectAttributes, PxFilterData,
                              PxPairFlags& pairs, const void*, PxU32) {
    pairs = PxPairFlag::eCONTACT_DEFAULT;
    return PxFilterFlag::eDEFAULT;
}

struct Context {
    PxDefaultAllocator allocator;
    Errors errors;
    PxFoundation* foundation = nullptr;
    PxPvd* debugger = nullptr;
    PxPvdTransport* transport = nullptr;
    PxPhysics* physics = nullptr;
    ImmediateDispatcher dispatcher;
    PxScene* scene = nullptr;
    PxScene* secondaryScene = nullptr;
    PxMaterial* material = nullptr;

    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    void connect(const std::filesystem::path& capture, const std::string& host = {}, unsigned port = 0) {
        foundation = PxCreateFoundation(PX_FOUNDATION_VERSION, allocator, errors);
        require(foundation != nullptr, "PxCreateFoundation failed");
        debugger = PxCreatePvd(*foundation);
        require(debugger != nullptr, "PxCreatePvd failed");
        transport = host.empty() ? PxDefaultPvdFileTransportCreate(capture.string().c_str())
            : PxDefaultPvdSocketTransportCreate(host.c_str(), static_cast<int>(port), 10000);
        require(transport != nullptr, "PVD transport creation failed");
        require(debugger->connect(*transport, PxPvdInstrumentationFlag::eDEBUG), "PVD transport connect failed");
    }

    void createSimulation() {
        physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), true, debugger);
        require(physics != nullptr, "PxCreatePhysics failed");
        PxSceneDesc description(physics->getTolerancesScale());
        description.gravity = PxVec3(0, -9.81f, 0);
        description.cpuDispatcher = &dispatcher;
        description.filterShader = collisionFilter;
        description.flags |= PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
        scene = physics->createScene(description);
        require(scene != nullptr, "createScene failed");
        if (auto* client = scene->getScenePvdClient()) {
            client->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONTACTS, true);
            client->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONSTRAINTS, true);
        }
        material = physics->createMaterial(0.5f, 0.5f, 0.05f);
        require(material != nullptr, "createMaterial failed");
    }

    ~Context() {
        if (secondaryScene) secondaryScene->release();
        if (scene) scene->release();
        if (material) material->release();
        if (physics) physics->release();
        if (debugger) { debugger->disconnect(); debugger->release(); }
        if (transport) transport->release();
        if (foundation) foundation->release();
    }
};

std::uint64_t instanceId(const void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::ofstream output(const std::filesystem::path& path) {
    std::ofstream file(path);
    require(bool(file), "Unable to open expected-value output");
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file << std::setprecision(9);
    return file;
}

PxRigidDynamic* box(Context& context, const char* name, const PxVec3& position, PxScene* target = nullptr) {
    auto* body = context.physics->createRigidDynamic(PxTransform(position));
    require(body != nullptr, "createRigidDynamic failed");
    auto* shape = context.physics->createShape(PxBoxGeometry(0.5f, 0.5f, 0.5f), *context.material, true);
    if (!shape) { body->release(); throw std::runtime_error("createShape failed"); }
    body->attachShape(*shape);
    shape->release();
    body->setMass(10.0f);
    // A unit cube has Ixx=Iyy=Izz=mass/6 about its center.
    body->setMassSpaceInertiaTensor(PxVec3(10.0f / 6.0f));
    body->setName(name); // Literals outlive the actor; the SDK does not copy names.
    body->setAngularDamping(0.5f);
    (target ? target : context.scene)->addActor(*body);
    return body;
}

void poseRow(std::ofstream& csv, unsigned step, const char* name, std::uint64_t id, const PxRigidActor* body,
             double timeSeconds = -1.0) {
    csv << step << ',' << (timeSeconds < 0.0 ? step * double(timestep) : timeSeconds)
        << ',' << name << ',' << id << ',' << (body ? 1 : 0);
    if (body) {
        const PxTransform transform = body->getGlobalPose();
        csv << ',' << transform.p.x << ',' << transform.p.y << ',' << transform.p.z
            << ',' << transform.q.x << ',' << transform.q.y << ',' << transform.q.z << ',' << transform.q.w;
    } else {
        csv << ",,,,,,,";
    }
    csv << '\n';
}

void check(pvd::PvdError error) {
    require(error == pvd::PvdErrorType::Success, "PvdDataStream operation failed");
}

struct StreamGuard {
    pvd::PvdDataStream* value;
    ~StreamGuard() { if (value) value->release(); }
};

void physicalCapture(const std::filesystem::path& prefix, unsigned steps,
                     const std::string& host = {}, unsigned port = 0,
                     bool twoScenes = false, unsigned pauseMs = 0) {
    auto expected = output(prefix.string() + "_expected.csv");
    auto frameExpected = output(prefix.string() + "_frame_expected.csv");
    auto lifecycle = output(prefix.string() + "_lifecycle.csv");
    expected << "step,time_seconds,object_name,instance_id,alive,px,py,pz,qx,qy,qz,qw\n";
    frameExpected << "step,time_seconds,object_name,instance_id,alive,px,py,pz,qx,qy,qz,qw\n";
    lifecycle << "before_step,event,object_name,instance_id\n";
    Context context;
    context.connect(prefix.string() + "_capture.pxd2", host, port);
    context.createSimulation();
    PxRigidDynamic* secondaryBox = nullptr;
    if (twoScenes) {
        PxSceneDesc description(context.physics->getTolerancesScale());
        description.gravity = PxVec3(0, -4.0f, 0);
        description.cpuDispatcher = &context.dispatcher;
        description.filterShader = collisionFilter;
        context.secondaryScene = context.physics->createScene(description);
        require(context.secondaryScene != nullptr, "secondary scene creation failed");
        secondaryBox = box(context, "other_scene_box", PxVec3(1, 5, 2), context.secondaryScene);
    }
    auto* validation = pvd::PvdDataStream::create(context.debugger);
    require(validation != nullptr, "PvdDataStream::create for validation markers failed");
    StreamGuard validationGuard{validation};
    auto* plane = context.physics->createRigidStatic(
        PxTransform(PxVec3(0), PxQuat(PxHalfPi, PxVec3(0, 0, 1))));
    require(plane != nullptr, "createRigidStatic failed");
    auto* planeShape = context.physics->createShape(PxPlaneGeometry(), *context.material, true);
    if (!planeShape) { plane->release(); throw std::runtime_error("create plane shape failed"); }
    plane->attachShape(*planeShape);
    planeShape->release();
    plane->setName("ground_plane");
    context.scene->addActor(*plane);
    auto* falling = box(context, "falling_box", PxVec3(0, 2, 0));
    PxRigidDynamic* transient = nullptr;
    const auto planeId = instanceId(plane);
    const auto fallingId = instanceId(falling);
    std::uint64_t transientId = 0;
    lifecycle << "0,create,ground_plane," << planeId << "\n0,create,falling_box," << fallingId << '\n';
    poseRow(expected, 0, "ground_plane", planeId, plane);
    poseRow(expected, 0, "falling_box", fallingId, falling);
    for (unsigned step = 1; step <= steps; ++step) {
        if (step == 6) {
            transient = box(context, "transient_box", PxVec3(4, 3, 0));
            transientId = instanceId(transient);
            lifecycle << step << ",create,transient_box," << transientId << '\n';
        }
        if (step == 16) {
            transient->release();
            transient = nullptr;
            lifecycle << step << ",destroy,transient_box," << transientId << '\n';
        }
        // PhysX 3.4 sends EndSection("frame") before current dynamic poses.
        // These values therefore describe the native frame boundary, including
        // lifecycle changes made before this simulation step.
        const double beforeTime = (step - 1) * double(timestep);
        poseRow(frameExpected, step, "ground_plane", planeId, plane, beforeTime);
        poseRow(frameExpected, step, "falling_box", fallingId, falling, beforeTime);
        poseRow(frameExpected, step, "transient_box", transientId, transient, beforeTime);
        context.scene->simulate(timestep);
        require(context.scene->fetchResults(true), "fetchResults failed");
        if (context.secondaryScene) {
            context.secondaryScene->simulate(timestep);
            require(context.secondaryScene->fetchResults(true), "secondary fetchResults failed");
        }
        poseRow(expected, step, "ground_plane", planeId, plane);
        poseRow(expected, step, "falling_box", fallingId, falling);
        if (transientId) poseRow(expected, step, "transient_box", transientId, transient);
        // A separate section name marks an unambiguous post-fetch event cursor
        // without changing the number of SDK "frame" sections.
        check(validation->beginSection(context.scene, "validation_after_fetch"));
        check(validation->endSection(context.scene, "validation_after_fetch"));
        if (pauseMs) std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs));
    }
    validationGuard.value->release();
    validationGuard.value = nullptr;
    lifecycle << steps + 1 << ",destroy,falling_box," << fallingId
              << '\n' << steps + 1 << ",destroy,ground_plane," << planeId << '\n';
    falling->release();
    plane->release();
    if (secondaryBox) secondaryBox->release();
    require(context.errors.failures == 0, "PhysX reported a simulation error");
    std::cout << "Physical capture: " << (host.empty() ? prefix.string() + "_capture.pxd2" : host + ":" + std::to_string(port))
              << " (" << steps << " simulation steps, " << (twoScenes ? 2 : 1) << " scenes)\n";
}

struct NodeUpdate {
    std::uint32_t value;
    PxVec3 position;
};
static_assert(sizeof(NodeUpdate) == 16, "Fixture wire message layout must be explicit");

void customCapture(const std::filesystem::path& prefix) {
    auto expected = output(prefix.string() + "_custom_expected.csv");
    expected << "step,object_name,instance_id,alive,value,x,y,z,parent_id,children_count,sample0,sample1,sample2\n";
    Context context;
    context.connect(prefix.string() + "_custom.pxd2");
    auto* stream = pvd::PvdDataStream::create(context.debugger);
    require(stream != nullptr, "PvdDataStream::create failed");
    // Release before Context tears down the connection, also on exceptions.
    StreamGuard guard{stream};
    const pvd::NamespacedName node("pvd_core_fixture", "Node");
    const pvd::NamespacedName derived("pvd_core_fixture", "DerivedNode");
    const pvd::NamespacedName message("pvd_core_fixture", "NodeUpdate");
    check(stream->createClass(node));
    auto property = [&](const char* name, const pvd::NamespacedName& type, pvd::PropertyType::Enum kind) {
        check(stream->createProperty(node, name, "", type, kind));
    };
    property("name", pvd::getPvdNamespacedNameForType<pvd::String>(), pvd::PropertyType::Scalar);
    property("value", pvd::getPvdNamespacedNameForType<pvd::PvdU32>(), pvd::PropertyType::Scalar);
    property("position", pvd::getPvdNamespacedNameForType<PxVec3>(), pvd::PropertyType::Scalar);
    property("samples", pvd::getPvdNamespacedNameForType<float>(), pvd::PropertyType::Array);
    property("parent", pvd::getPvdNamespacedNameForType<pvd::ObjectRef>(), pvd::PropertyType::Scalar);
    property("children", pvd::getPvdNamespacedNameForType<pvd::ObjectRef>(), pvd::PropertyType::Array);
    // Derivation locks the parent's layout in the real SDK.
    check(stream->createClass(derived));
    check(stream->deriveClass(node, derived));
    pvd::PropertyMessageArg args[] = {
        {"value", pvd::getPvdNamespacedNameForType<pvd::PvdU32>(), offsetof(NodeUpdate, value), sizeof(std::uint32_t)},
        {"position", pvd::getPvdNamespacedNameForType<PxVec3>(), offsetof(NodeUpdate, position), sizeof(PxVec3)}
    };
    check(stream->createPropertyMessage(node, message, pvd::DataRef<pvd::PropertyMessageArg>(args, 2), sizeof(NodeUpdate)));
    std::array<int, 2> keys{};
    void* root = &keys[0];
    void* child = &keys[1];
    check(stream->createInstance(node, root));
    check(stream->createInstance(derived, child));
    const char* rootName = "fixture_root";
    const char* childName = "fixture_child";
    check(stream->setPropertyValue(root, "name", rootName));
    check(stream->setPropertyValue(child, "name", childName));
    check(stream->setPropertyValue(child, "parent", root));
    check(stream->pushBackObjectRef(root, "children", child));
    const float initial[] = {1, 2, 3};
    check(stream->beginSetPropertyValue<float>(root, "samples"));
    check(stream->appendPropertyValueData(initial, 1));
    check(stream->appendPropertyValueData(initial + 1, 2));
    check(stream->endSetPropertyValue());
    for (unsigned step = 1; step <= 12; ++step) {
        if (step == 10) {
            check(stream->removeObjectRef(root, "children", child));
            check(stream->destroyInstance(child));
        }
        check(stream->beginSection(root, "frame"));
        const float samples[] = {float(step), float(step) + 0.25f, -float(step)};
        check(stream->setPropertyValue(root, "samples", samples, 3));
        const NodeUpdate rootUpdate{100 + step, PxVec3(float(step), 2, 3)};
        const NodeUpdate childUpdate{200 + step, PxVec3(float(step), 4, 5)};
        if (step % 2) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&rootUpdate);
            check(stream->setPropertyMessage(root, message, pvd::DataRef<const std::uint8_t>(bytes, sizeof(rootUpdate))));
            if (step < 10) {
                bytes = reinterpret_cast<const std::uint8_t*>(&childUpdate);
                check(stream->setPropertyMessage(child, message, pvd::DataRef<const std::uint8_t>(bytes, sizeof(childUpdate))));
            }
        } else {
            check(stream->beginPropertyMessageGroup(message));
            check(stream->sendPropertyMessageFromGroup(root, rootUpdate));
            if (step < 10) check(stream->sendPropertyMessageFromGroup(child, childUpdate));
            check(stream->endPropertyMessageGroup());
        }
        check(stream->endSection(root, "frame"));
        expected << step << ",fixture_root," << instanceId(root) << ",1," << 100 + step << ',' << step
                 << ",2,3,0," << (step < 10 ? 1 : 0) << ',' << samples[0] << ',' << samples[1] << ',' << samples[2] << '\n';
        expected << step << ",fixture_child," << instanceId(child) << ',' << (step < 10 ? 1 : 0);
        if (step < 10) expected << ',' << 200 + step << ',' << step << ",4,5," << instanceId(root) << ",0,,,";
        else expected << ",,,,,,,,,";
        expected << '\n';
    }
    check(stream->destroyInstance(root));
    require(context.errors.failures == 0, "PhysX reported a custom capture error");
    std::cout << "Custom capture: " << prefix.string() << "_custom.pxd2 (12 sections)\n";
}

unsigned parseSteps(const char* argument) {
    unsigned value = 0;
    const std::string text(argument);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value >= 20 && value <= 10000,
            "steps must be an integer in [20, 10000]");
    return value;
}
} // namespace

int main(int argc, char** argv) {
    Assertions assertions;
    PxSetAssertHandler(assertions);
    try {
        if (argc > 1 && std::string(argv[1]) == "--tcp") {
            require(argc == 6 || argc == 7, "TCP usage: --tcp host port output-prefix steps [pause-ms]");
            const auto port = std::stoul(argv[3]);
            require(port > 0 && port <= 65535, "invalid TCP port");
            const auto steps = parseSteps(argv[5]);
            const auto pause = argc == 7 ? std::stoul(argv[6]) : 1ul;
            require(pause <= 100, "pause-ms must be <= 100");
            const std::filesystem::path prefix(argv[4]);
            if (!prefix.parent_path().empty()) std::filesystem::create_directories(prefix.parent_path());
            physicalCapture(prefix, steps, argv[2], static_cast<unsigned>(port), true, static_cast<unsigned>(pause));
            return 0;
        }
        if (argc > 3 || (argc > 1 && std::string(argv[1]) == "--help")) {
            std::cout << "Usage: pvd_physx34_capture [output-prefix] [steps=48]\n"
                         "       pvd_physx34_capture --tcp host port output-prefix steps [pause-ms=1]\n"
                         "TCP mode streams two scenes; file mode also generates custom event fixtures.\n"
                         "Creates raw SDK .pxd2 streams and expected CSVs in the selected output directory.\n"
                         "*_expected.csv: step 0 is initial state; step N is after fetchResults number N.\n"
                         "*_frame_expected.csv: step N is before simulate number N, after lifecycle changes.\n"
                         "validation_after_fetch sections mark the post-fetch state without adding frames.\n";
            return argc > 3 ? 2 : 0;
        }
        const std::filesystem::path prefix = argc > 1 ? argv[1] : "physx34_capture";
        const auto directory = prefix.parent_path();
        if (!directory.empty()) std::filesystem::create_directories(directory);
        const unsigned steps = argc > 2 ? parseSteps(argv[2]) : 48;
        physicalCapture(prefix, steps);
        customCapture(prefix);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Capture failed: " << error.what() << '\n';
        return 1;
    }
}
