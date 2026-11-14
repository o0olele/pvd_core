// Real eMEMORY PvdMemClient fixture. This executable never links pvd_core.
// The SDK's broadcasting allocator notifies the same listener used by PhysX.
// Two preallocated aligned blocks guarantee address reuse independently of CRT
// heap behavior. CSV expectations are recorded at the allocator callback.
#include <PsAllocator.h>
#include <PsPvd.h>
#include <PxPhysicsAPI.h>
#include <PxPvdDataStream.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace physx;
namespace sdk = physx::pvdsdk;
void need(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
struct Assertions final : PxAssertHandler {
  void operator()(const char *expr, const char *file, int line,
                  bool &) override {
    std::fprintf(stderr, "PhysX assertion: %s (%s:%d)\n", expr, file, line);
    std::fflush(nullptr);
    std::_Exit(3);
  }
};
struct Errors final : PxErrorCallback {
  unsigned failures = 0;
  void reportError(PxErrorCode::Enum code, const char *message, const char *,
                   int) override {
    std::fprintf(stderr, "PhysX [%u] %s\n", unsigned(code), message);
    if (code != PxErrorCode::eDEBUG_INFO &&
        code != PxErrorCode::eDEBUG_WARNING &&
        code != PxErrorCode::ePERF_WARNING)
      ++failures;
  }
};
struct Expected {
  void *address;
  std::size_t generation, size;
  std::string type, file;
  int line;
  bool active = true;
};
struct Allocator final : PxAllocatorCallback {
  struct Slot {
    void *pointer = nullptr;
    bool active = false;
    std::size_t generation = 0;
  };
  PxDefaultAllocator fallback;
  std::array<Slot, 2> slots;
  std::vector<Expected> expected;
  std::uint64_t current = 0, peak = 0, allocated = 0, freed = 0;
  std::size_t alloc_count = 0, free_count = 0;
  Allocator() {
    for (auto &s : slots) {
      s.pointer =
          fallback.allocate(1024 * 1024, "RawFixtureBlock", __FILE__, __LINE__);
      need(s.pointer, "fixture backing allocation");
    }
  }
  ~Allocator() {
    for (auto &s : slots)
      fallback.deallocate(s.pointer);
  }
  void *allocate(std::size_t size, const char *type, const char *file,
                 int line) override {
    if (type && std::strncmp(type, "MemoryFixture.", 14) == 0) {
      need(size <= 1024 * 1024, "fixture allocation exceeds block");
      for (auto &slot : slots)
        if (!slot.active) {
          slot.active = true;
          expected.push_back({slot.pointer, slot.generation++, size, type,
                              file ? file : "", line, true});
          current += size;
          allocated += size;
          peak = std::max(peak, current);
          ++alloc_count;
          return slot.pointer;
        }
      throw std::runtime_error("fixture slots exhausted");
    }
    return fallback.allocate(size, type, file, line);
  }
  void deallocate(void *address) override {
    for (auto &slot : slots)
      if (slot.pointer == address) {
        need(slot.active, "fixture duplicate free");
        slot.active = false;
        for (auto it = expected.rbegin(); it != expected.rend(); ++it)
          if (it->address == address && it->active) {
            it->active = false;
            current -= it->size;
            freed += it->size;
            ++free_count;
            return;
          }
        throw std::runtime_error("fixture missing expected allocation");
      }
    fallback.deallocate(address);
  }
  void write_phase(std::ostream &rows, std::ostream &totals, unsigned phase) {
    for (const auto &a : expected)
      rows << phase << ',' << reinterpret_cast<std::uintptr_t>(a.address) << ','
           << a.generation << ',' << a.size << ',' << a.type << ',' << a.file
           << ',' << a.line << ',' << a.active << '\n';
    totals << phase << ',' << current << ',' << peak << ',' << allocated << ','
           << freed << ',' << alloc_count << ',' << free_count << '\n';
  }
};
struct Context {
  Allocator allocator;
  Errors errors;
  PxFoundation *foundation = nullptr;
  PxPvd *pvd = nullptr;
  PxPvdTransport *transport = nullptr;
  PxPhysics *physics = nullptr;
  sdk::PvdDataStream *stream = nullptr;
  ~Context() {
    if (stream)
      stream->release();
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
  void open(const std::filesystem::path &path) {
    foundation = PxCreateFoundation(PX_FOUNDATION_VERSION, allocator, errors);
    need(foundation, "foundation");
    pvd = PxCreatePvd(*foundation);
    need(pvd, "pvd");
    transport = PxDefaultPvdFileTransportCreate(path.string().c_str());
    need(transport, "transport");
    need(pvd->connect(*transport, PxPvdInstrumentationFlag::eDEBUG |
                                      PxPvdInstrumentationFlag::eMEMORY),
         "connect");
    physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation,
                              PxTolerancesScale(), true, pvd);
    need(physics, "physics");
    // NpPhysics registers PvdImpl as the Foundation AllocationListener.
    stream = sdk::PvdDataStream::create(pvd);
    need(stream, "data stream");
  }
  void flush() { static_cast<sdk::PsPvd *>(pvd)->flush(); }
};
} // namespace
int main(int argc, char **argv) {
  try {
    const std::filesystem::path prefix = argc > 1 ? argv[1] : "memory";
    if (!prefix.parent_path().empty())
      std::filesystem::create_directories(prefix.parent_path());
    std::ofstream expected(prefix.string() + "_memory_expected.csv"),
        summary(prefix.string() + "_memory_summary.csv");
    need(bool(expected) && bool(summary), "expected CSV output");
    expected << "phase,address,generation,size,type,file,line,active\n";
    summary << "phase,current_bytes,peak_bytes,allocated_bytes,freed_bytes,"
               "allocation_count,free_count\n";
    Assertions assertions;
    PxSetAssertHandler(assertions);
    Context c;
    c.open(prefix.string() + "_memory.pxd2");
    auto &broadcaster = physx::shdfnd::getAllocator();
    void *a = nullptr, *b = nullptr, *d = nullptr;
    for (unsigned phase = 0; phase != 5; ++phase) {
      std::string name = "memory_validation_" + std::to_string(phase);
      need(c.stream->beginSection(&c, name.c_str()) ==
               sdk::PvdErrorType::Success,
           "begin section");
      switch (phase) {
      case 0:
        a = broadcaster.allocate(64, "MemoryFixture.A", "fixture_a.cpp", 12);
        b = broadcaster.allocate(512, "MemoryFixture.B", "fixture_b.cpp", 345);
        break;
      case 1:
        broadcaster.deallocate(a);
        a = broadcaster.allocate(70000, "MemoryFixture.C", "fixture_c.cpp",
                                 70000);
        break;
      case 2:
        broadcaster.deallocate(b);
        d = broadcaster.allocate(8192, "MemoryFixture.D", "fixture_d.cpp", 999);
        break;
      case 3:
        broadcaster.deallocate(d);
        break;
      case 4:
        broadcaster.deallocate(a);
        break;
      }
      c.flush();
      std::string message = "MemoryFixture phase " + std::to_string(phase);
      c.stream->sendErrorMessage(
          phase % 2 ? PxErrorCode::ePERF_WARNING : PxErrorCode::eDEBUG_WARNING,
          message.c_str(), "memory_fixture.cpp", 100 + phase);
      need(c.stream->endSection(&c, name.c_str()) == sdk::PvdErrorType::Success,
           "end section");
      c.allocator.write_phase(expected, summary, phase);
    }
    c.flush();
    need(c.errors.failures == 0, "SDK callback failure");
    need(bool(expected) && bool(summary), "write expected CSV");
    std::cout << "real PvdMemClient: 5 phases, 4 fixture allocations, 2 "
                 "guaranteed address reuses, 5 ErrorMessage events\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
