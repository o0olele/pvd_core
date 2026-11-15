#include "pvd/deformables.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using namespace pvd;
namespace d = pvd::deformables;
bool shared_input = false;
void check(bool b, const std::string &m) {
  if (!b)
    throw std::runtime_error(m);
}
void near(float a, float b, const char *m) {
  check(std::isfinite(a) && std::isfinite(b) &&
            std::abs(a - b) <= 3e-5f + std::abs(b) * 4e-6f,
        m);
}
void near(Vec3 a, Vec3 b, const char *m) {
  near(a.x, b.x, m);
  near(a.y, b.y, m);
  near(a.z, b.z, m);
}
Bytes ints(std::initializer_list<std::uint64_t> values, std::size_t width,
           ByteOrder order) {
  Bytes b;
  for (auto value : values)
    for (std::size_t i = 0; i < width; ++i)
      b.push_back(static_cast<std::uint8_t>(
          value >> ((order == ByteOrder::little ? i : width - i - 1) * 8)));
  return b;
}
Bytes floats(std::initializer_list<float> values, ByteOrder order) {
  Bytes b;
  for (auto v : values) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    auto field = ints({bits}, 4, order);
    b.insert(b.end(), field.begin(), field.end());
  }
  return b;
}
struct Fixture {
  Metadata metadata;
  Snapshot state;
  ByteOrder order;
  std::uint8_t pointers;
  Fixture(ByteOrder o = ByteOrder::little, std::uint8_t ptr = 8)
      : order(o), pointers(ptr) {
    for (const char *n :
         {"PxScene", "PxActor", "PxCloth", "PxClothFabric", "PxParticleBase",
          "PxParticleSystem", "PxParticleFluid"}) {
      QualifiedName q{"physx3", n};
      metadata.classes.emplace(q, ClassDefinition{q, {}, {}});
    }
    metadata.classes.at({"physx3", "PxCloth"}).parent =
        QualifiedName{"physx3", "PxActor"};
    metadata.classes.at({"physx3", "PxParticleBase"}).parent =
        QualifiedName{"physx3", "PxActor"};
    for (const char *n : {"PxParticleSystem", "PxParticleFluid"})
      metadata.classes.at({"physx3", n}).parent =
          QualifiedName{"physx3", "PxParticleBase"};
    object(1, "PxScene").generation = 2;
    object(2, "PxScene");
    object(10, "PxCloth").generation = 3;
    ref(10, "Scene", 1);
    ref(10, "Fabric", 100);
    u(10, "NbParticles", 4);
    u(10, "ActorFlags", 0);
    u(10, "ClothFlags", 0);
    u(10, "IsSleeping", 0, 1, "PvdBool");
    // Rz(90deg), translation(10,20,30), SDK quaternion-then-position encoding.
    f(10, "GlobalPose", "PxTransform",
      {0, 0, .70710677f, .70710677f, 10, 20, 30}, 1);
    f(10, "ParticleBuffer", "PxClothParticle",
      {0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 2}, 4);
    object(100, "PxClothFabric").generation = 4;
    u(100, "NbParticles", 4);
    put(100, "ParticleIndices", "PvdU32", ints({0, 1, 1, 2, 2, 3}, 4, order),
        6);
    put(100, "Sets", "PvdU32", ints({1, 3}, 4, order), 2);
    f(100, "Restvalues", "PvdF32", {1, 1.41421356f, 1.7320508f}, 3);
    put(100, "Phases", "PxClothFabricPhase", ints({1, 0, 4, 1}, 4, order), 2);
    particle(20, "PxParticleSystem");
    particle(21, "PxParticleFluid");
    f(21, "Densities", "PvdF32", {0, 1, 2, 3}, 4);
    state.event_count = 123;
  }
  InstanceState &object(std::uint64_t id, const char *n) {
    auto &v = state.instances[id];
    v.id = id;
    v.class_name = {"physx3", n};
    return v;
  }
  void put(std::uint64_t id, const char *p, const char *type, Bytes b,
           std::uint64_t count) {
    PropertyValue v;
    v.incoming_type = {"physx3", type};
    v.item_count = count;
    v.byte_order = order;
    v.pointer_width = pointers;
    v.bytes = std::make_shared<const Bytes>(std::move(b));
    state.instances.at(id).properties[p] = std::move(v);
  }
  void u(std::uint64_t id, const char *p, std::uint64_t v,
         std::size_t width = 4, const char *t = nullptr) {
    put(id, p,
        t            ? t
        : width == 1 ? "PvdU8"
        : width == 2 ? "PvdU16"
        : width == 4 ? "PvdU32"
                     : "PvdU64",
        ints({v}, width, order), 1);
  }
  void f(std::uint64_t id, const char *p, const char *t,
         std::initializer_list<float> v, std::uint64_t count) {
    put(id, p, t, floats(v, order), count);
  }
  void ref(std::uint64_t id, const char *p, std::uint64_t target) {
    put(id, p, "VoidPtr", ints({target}, pointers, order), 1);
    state.instances.at(id).properties.at(p).reference_ids =
        std::vector<std::uint64_t>{target};
  }
  void particle(std::uint64_t id, const char *t) {
    object(id, t);
    ref(id, "Scene", 1);
    u(id, "ActorFlags", 0);
    u(id, "ParticleBaseFlags", 4);
    u(id, "ParticleReadDataFlags", 91);
    u(id, "MaxParticles", 96);
    u(id, "NbParticles", 4);
    u(id, "ValidParticleRange", 64);
    put(id, "ValidParticleBitmap", "PvdU32",
        ints({0x80000001u, 0x80000001u, 0}, 4, order), 3);
    f(id, "Positions", "PxVec3", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, 4);
    f(id, "Velocities", "PxVec3",
      {-.1f, 0, 0, -.2f, 0, 0, -.3f, 0, 0, -.4f, 0, 0}, 4);
    put(id, "Flags", "PvdU16", ints({1, 3, 5, 1}, 2, order), 4);
    f(id, "CollisionNormals", "PxVec3",
      {NAN, NAN, NAN, 0, 1, 0, 1, 0, 0, NAN, NAN, NAN}, 4);
    f(id, "RestOffsets", "PvdF32", {0, .01f, .02f, .03f}, 4);
  }
  d::Frame extract(const d::Options &o = {}) const {
    if (shared_input)
      return d::extract(metadata, SnapshotView(state), o);
    return d::extract(metadata, state, o);
  }
};
bool diagnostic(const d::Frame &f, d::DiagnosticCode c) {
  return std::any_of(f.diagnostics.begin(), f.diagnostics.end(),
                     [&](const d::Diagnostic &v) { return v.code == c; });
}
const d::ParticleCloud &cloud(const d::Frame &f, std::uint64_t id) {
  auto i = std::find_if(
      f.particle_clouds.begin(), f.particle_clouds.end(),
      [&](const d::ParticleCloud &c) { return c.key.owner.id == id; });
  check(i != f.particle_clouds.end(), "cloud exists");
  return *i;
}
void valid(ByteOrder order, std::uint8_t pointers) {
  Fixture fixture(order, pointers);
  auto result = fixture.extract();
  check(result.complete && result.event_count == 123 &&
            result.cloths.size() == 1 && result.particle_clouds.size() == 2,
        "native owners only");
  const auto &c = result.cloths[0];
  check(c.key.owner == d::ObjectKey{10, 3} &&
            c.key.scene == d::ObjectKey{1, 2} &&
            c.fabric == d::ObjectKey{100, 4},
        "scene/owner/fabric generations");
  check(c.positions_status == d::DataStatus::available &&
            c.local_positions.size() == 4 &&
            c.pinned_indices == std::vector<std::uint32_t>({0, 2}),
        "cloth pins and count");
  near(c.world_positions[1], {10, 21, 30}, "cloth local to world");
  near(c.world_positions[3], {9, 21, 31}, "cloth local to world z");
  check(c.constraints.size() == 3 && c.constraints[0].set_index == 0u &&
            c.constraints[2].set_index == 1u && c.phases[1].phase_type == 4,
        "fabric constraints and phase mapping");
  near(*c.constraints[1].rest_length, 1.41421356f, "constraint rest length");
  check(c.triangle_indices.empty() &&
            c.triangle_topology_status == d::DataStatus::not_recorded &&
            diagnostic(result, d::DiagnosticCode::topology_not_recorded),
        "never invent cloth faces");
  for (const auto &p : result.particle_clouds) {
    check(p.indices_status == d::DataStatus::available &&
              p.particles.size() == 4 && !p.updates_disabled,
          "enabled particles");
    for (std::size_t i = 0; i < 4; ++i)
      check(p.particles[i].index == std::array<unsigned, 4>{0, 31, 32, 63}[i],
            "sparse indices retain original IDs");
    near(*p.particles[2].position, {7, 8, 9}, "packed world position");
    check(!p.particles[0].collision_normal && p.particles[1].collision_normal &&
              !p.particles[3].collision_normal,
          "undefined noncollision normals ignored");
    check(p.world_bounds.has_value(), "particle bounds");
  }
  check(cloud(result, 21).kind == d::ParticleKind::fluid &&
            cloud(result, 21).particles[0].density == 0,
        "zero fluid density valid");
  d::Options options;
  options.owners = {{20, 0}};
  check(fixture.extract(options).particle_clouds.size() == 1,
        "owner selection");
  fixture.state.instances.at(10).properties.erase("Scene");
  check(fixture.extract(options).complete,
        "unselected owner cannot pollute diagnostics");
  options.owners = {{20, 1}};
  result = fixture.extract(options);
  check(result.particle_clouds.empty() && !result.complete &&
            diagnostic(result, d::DiagnosticCode::dangling_reference),
        "stale owner generation diagnosed");
  options = {};
  options.scene = d::ObjectKey{1, 99};
  check(!fixture.extract(options).complete, "stale scene generation diagnosed");
  fixture.state.instances.erase(20);
  check(fixture.extract().particle_clouds.size() == 1,
        "destroyed owner absent");
  fixture.particle(20, "PxParticleSystem");
  fixture.state.instances.at(20).generation = 1;
  options = {};
  options.owners = {{20, 1}};
  check(fixture.extract(options).particle_clouds[0].key.owner.generation == 1,
        "recreated owner generation");
}
void malformed() {
  auto test = [](auto mutate, d::DiagnosticCode code) {
    Fixture f;
    mutate(f);
    auto result = f.extract();
    check(!result.complete && diagnostic(result, code),
          "malformed data diagnosed");
    return result;
  };
  test([](Fixture &f) { f.u(20, "NbParticles", 4, 4, "StringHandle"); },
       d::DiagnosticCode::invalid_type);
  test([](Fixture &f) { f.put(20, "ValidParticleBitmap", "PvdU32", {1}, 3); },
       d::DiagnosticCode::count_mismatch);
  test([](Fixture &f) { f.u(20, "NbParticles", 3); },
       d::DiagnosticCode::count_mismatch);
  test([](Fixture &f) { f.u(20, "NbParticles", 0); },
       d::DiagnosticCode::count_mismatch);
  test([](Fixture &f) { f.u(20, "ValidParticleRange", 97); },
       d::DiagnosticCode::count_mismatch);
  test(
      [](Fixture &f) {
        f.state.instances.at(20).properties.erase("ValidParticleBitmap");
      },
      d::DiagnosticCode::missing_property);
  auto result =
      test([](Fixture &f) { f.f(20, "Positions", "PxVec3", {1, 2, NAN}, 1); },
           d::DiagnosticCode::count_mismatch);
  check(!cloud(result, 20).particles[0].position &&
            !cloud(result, 20).world_bounds,
        "bad positions are atomic");
  result = test(
      [](Fixture &f) {
        f.f(20, "Velocities", "PxVec3",
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, INFINITY, 0}, 4);
      },
      d::DiagnosticCode::invalid_value);
  check(cloud(result, 20).velocities_status == d::DataStatus::malformed &&
            !cloud(result, 20).particles[0].velocity,
        "nonfinite velocities no partial output");
  test([](Fixture &f) { f.f(21, "Densities", "PvdF32", {0, 1, -1, 2}, 4); },
       d::DiagnosticCode::invalid_value);
  test([](Fixture &f) { f.f(20, "RestOffsets", "PvdF32", {0, 1, -1, 2}, 4); },
       d::DiagnosticCode::invalid_value);
  result = test(
      [](Fixture &f) {
        f.put(20, "Flags", "PvdU16", ints({1, 3, 4, 1}, 2, f.order), 4);
      },
      d::DiagnosticCode::invalid_value);
  check(cloud(result, 20).flags_status == d::DataStatus::malformed &&
            !cloud(result, 20).particles[0].flags,
        "bad flags are atomic");
  test(
      [](Fixture &f) {
        f.f(10, "ParticleBuffer", "PxClothParticle",
            {0, 0, 0, -1, 1, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1}, 4);
      },
      d::DiagnosticCode::invalid_value);
  test(
      [](Fixture &f) {
        f.f(10, "GlobalPose", "PxTransform", {0, 0, 0, 0, 10, 20, 30}, 1);
      },
      d::DiagnosticCode::invalid_value);
  result = test(
      [](Fixture &f) {
        f.put(100, "ParticleIndices", "PvdU32",
              ints({0, 1, 1, 9, 2, 3}, 4, f.order), 6);
      },
      d::DiagnosticCode::invalid_index);
  check(result.cloths[0].world_positions.size() == 4 &&
            result.cloths[0].constraints.empty(),
        "bad fabric does not discard valid cloth points");
  test(
      [](Fixture &f) {
        f.put(100, "Sets", "PvdU32", ints({3, 1}, 4, f.order), 2);
      },
      d::DiagnosticCode::invalid_index);
  test(
      [](Fixture &f) {
        f.put(100, "Phases", "PxClothFabricPhase", ints({99, 0}, 4, f.order),
              1);
      },
      d::DiagnosticCode::invalid_value);
  test([](Fixture &f) { f.ref(10, "Fabric", 999); },
       d::DiagnosticCode::dangling_reference);
  test(
      [](Fixture &f) {
        f.state.instances.at(20).properties.at("Scene").reference_ids =
            std::vector<std::uint64_t>{};
      },
      d::DiagnosticCode::invalid_type);
  Fixture f;
  f.u(20, "NbParticles", 0);
  f.u(20, "ValidParticleRange", 0);
  // SDK deliberately leaves old arrays untouched on an empty update.
  f.put(20, "ValidParticleBitmap", "wrong", {0}, 999);
  f.put(20, "CollisionNormals", "wrong", {0}, 999);
  result = f.extract();
  check(result.complete && cloud(result, 20).particles.empty() &&
            !cloud(result, 20).world_bounds,
        "empty count/range ignores stale buffers including invalid normals");
  f = Fixture{};
  f.u(20, "ParticleReadDataFlags", 0);
  f.state.instances.at(20).properties.erase("Positions");
  f.state.instances.at(20).properties.erase("Velocities");
  result = f.extract();
  check(cloud(result, 20).positions_status == d::DataStatus::disabled &&
            cloud(result, 20).velocities_status == d::DataStatus::disabled,
        "absent disabled read buffers explicit");
  f = Fixture{};
  f.u(20, "ParticleReadDataFlags", 0);
  result = f.extract();
  check(cloud(result, 20).positions_status == d::DataStatus::available,
        "captured CPU frame arrays override disabled API read flags");
  f.u(20, "ParticleBaseFlags", 0);
  result = f.extract();
  check(cloud(result, 20).updates_disabled, "stale disabled owner marked");
  f = Fixture{};
  f.u(20, "ValidParticleRange", 63);
  result = f.extract();
  check(!result.complete &&
            diagnostic(result, d::DiagnosticCode::count_mismatch),
        "padding cannot add particle");
  f = Fixture{};
  f.put(20, "ValidParticleBitmap", "PvdU32",
        ints({0x80000001u, 0x80000001u, 1}, 4, f.order), 3);
  result = f.extract();
  check(cloud(result, 20).particles.size() == 4 &&
            diagnostic(result, d::DiagnosticCode::padding_bits),
        "SDK extra padding word never makes particle");
  f = Fixture{};
  f.state.instances.at(10).properties.erase("ParticleBuffer");
  result = f.extract();
  check(result.cloths[0].positions_status == d::DataStatus::not_recorded &&
            result.cloths[0].world_positions.empty(),
        "initial cloth missing buffer explicit");
  f = Fixture{};
  f.state.instances.at(10).properties.erase("GlobalPose");
  result = f.extract();
  check(result.cloths[0].local_positions_status == d::DataStatus::available &&
            result.cloths[0].positions_status == d::DataStatus::not_recorded &&
            result.cloths[0].world_positions.empty(),
        "no identity substitute for missing cloth pose");
  d::Options o;
  o.max_particles = 2;
  result = Fixture{}.extract(o);
  check(diagnostic(result, d::DiagnosticCode::resource_limit),
        "particle resource bound");
  o = {};
  o.max_cloth_vertices = 3;
  result = Fixture{}.extract(o);
  check(diagnostic(result, d::DiagnosticCode::resource_limit),
        "cloth resource bound");
  o = {};
  o.max_constraints = 2;
  result = Fixture{}.extract(o);
  check(diagnostic(result, d::DiagnosticCode::resource_limit),
        "constraint resource bound");
  o = {};
  o.max_valid_range = 32;
  result = Fixture{}.extract(o);
  check(diagnostic(result, d::DiagnosticCode::resource_limit),
        "sparse index range bound");
  o = {};
  o.max_bitmap_words = 2;
  result = Fixture{}.extract(o);
  check(diagnostic(result, d::DiagnosticCode::resource_limit),
        "bitmap word bound");
  o = {};
  o.max_property_bytes = 4;
  result = Fixture{}.extract(o);
  check(diagnostic(result, d::DiagnosticCode::resource_limit),
        "property byte bound");
  o = {};
  o.max_diagnostics = 1;
  o.max_owners = 1;
  result = Fixture{}.extract(o);
  check(result.diagnostics.size() == 1 && result.suppressed_diagnostics > 0 &&
            !result.complete,
        "bounded diagnostics");
  o = {};
  o.max_particles = 0;
  bool threw = false;
  try {
    Fixture{}.extract(o);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  check(threw, "invalid limits explicit");
}

using Row = std::vector<std::string>;
Row split(const std::string &s) {
  Row out;
  std::size_t at = 0;
  for (;;) {
    auto end = s.find(',', at);
    out.push_back(s.substr(at, end - at));
    if (end == std::string::npos)
      break;
    at = end + 1;
  }
  return out;
}
std::uint64_t u(const Row &r, std::size_t at) { return std::stoull(r.at(at)); }
float f(const Row &r, std::size_t at) { return std::stof(r.at(at)); }
Vec3 v(const Row &r, std::size_t at) {
  return {f(r, at), f(r, at + 1), f(r, at + 2)};
}
struct OracleOwner {
  Row row;
  std::map<unsigned, Row> particles;
};
struct OracleCloth {
  Row row;
  std::map<unsigned, Row> vertices, constraints, phases;
};
struct OracleSample {
  std::map<std::uint64_t, OracleOwner> particles;
  std::map<std::uint64_t, OracleCloth> cloths;
};
void sdk(const std::string &path, const std::string &oracle_path) {
  std::ifstream file(oracle_path);
  check(bool(file), "SDK CSV input");
  std::map<unsigned, OracleSample> samples;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    auto r = split(line);
    auto &sample = samples[static_cast<unsigned>(u(r, 1))];
    if (r[0] == "owner")
      sample.particles[u(r, 4)].row = std::move(r);
    else if (r[0] == "particle")
      sample.particles[u(r, 2)].particles.emplace(
          static_cast<unsigned>(u(r, 3)), std::move(r));
    else if (r[0] == "cloth_owner")
      sample.cloths[u(r, 3)].row = std::move(r);
    else if (r[0] == "cloth_vertex")
      sample.cloths[u(r, 2)].vertices.emplace(static_cast<unsigned>(u(r, 3)),
                                              std::move(r));
    else if (r[0] == "cloth_constraint")
      sample.cloths[u(r, 2)].constraints.emplace(static_cast<unsigned>(u(r, 3)),
                                                 std::move(r));
    else if (r[0] == "cloth_phase")
      sample.cloths[u(r, 2)].phases.emplace(static_cast<unsigned>(u(r, 3)),
                                            std::move(r));
    else
      check(false, "unknown CSV row");
  }
  auto session = Session::load_file(path);
  std::size_t checked_particles = 0, checked_vertices = 0, checked_edges = 0;
  auto compare = [&](unsigned step, const Snapshot &state) {
    auto frame = shared_input ? d::extract(session.metadata(), SnapshotView(state))
                              : d::extract(session.metadata(), state);
    const auto &expected = samples.at(step);
    check(frame.particle_clouds.size() == expected.particles.size() &&
              frame.cloths.size() == expected.cloths.size(),
          "SDK native owner count");
    for (const auto &cloud : frame.particle_clouds) {
      const auto &o = expected.particles.at(cloud.key.owner.id);
      const auto &row = o.row;
      check(cloud.key.owner.generation ==
                    state.instance(cloud.key.owner.id)->generation &&
                cloud.key.scene.id == u(row, 3) &&
                cloud.key.scene.generation ==
                    state.instance(u(row, 3))->generation,
            "SDK particle scene/owner generation");
      check(cloud.read_flags == u(row, 5) && cloud.base_flags == u(row, 6) &&
                cloud.max_particles == u(row, 7) &&
                cloud.declared_count == u(row, 8) &&
                cloud.valid_range == u(row, 9) && cloud.name == row[10],
            "SDK particle metadata");
      check(cloud.indices_status == d::DataStatus::available &&
                cloud.particles.size() == o.particles.size(),
            "SDK packed sparse count");
      check(cloud.updates_disabled == ((u(row, 6) & 4) == 0),
            "SDK particle paused status");
      check(cloud.positions_status == d::DataStatus::available,
            "SDK particle positions available");
      const bool velocity_present = step != 0 || (u(row, 5) & 2) != 0;
      check(cloud.velocities_status == (velocity_present
                                            ? d::DataStatus::available
                                            : d::DataStatus::disabled),
            "SDK read flag/frame velocity availability");
      if (cloud.particles.empty())
        check(!cloud.world_bounds, "SDK zero-count stale arrays ignored");
      for (const auto &p : cloud.particles) {
        const auto &r = o.particles.at(p.index);
        check(p.position.has_value(), "SDK particle position");
        near(*p.position, v(r, 4), "SDK world particle position");
        if (velocity_present) {
          check(p.velocity.has_value(), "SDK particle velocity");
          near(*p.velocity, v(r, 7), "SDK particle velocity oracle");
        } else
          check(!p.velocity, "SDK missing velocity explicit");
        check(p.flags == u(r, 10), "SDK particle flags");
        if (!r[11].empty()) {
          check(p.rest_offset.has_value(), "SDK rest offset");
          near(*p.rest_offset, f(r, 11), "SDK rest offset oracle");
        }
        if (!r[12].empty()) {
          check(p.density.has_value(), "SDK density");
          near(*p.density, f(r, 12), "SDK density oracle");
        }
        ++checked_particles;
      }
    }
    for (const auto &cloth : frame.cloths) {
      const auto &o = expected.cloths.at(cloth.key.owner.id);
      const auto &row = o.row;
      check(cloth.key.scene.id == u(row, 2) &&
                cloth.key.scene.generation ==
                    state.instance(u(row, 2))->generation &&
                cloth.key.owner.generation ==
                    state.instance(cloth.key.owner.id)->generation,
            "SDK cloth scene/owner generation");
      check(cloth.fabric && cloth.fabric->id == u(row, 4) &&
                cloth.fabric->generation ==
                    state.instance(u(row, 4))->generation &&
                cloth.declared_count == u(row, 5),
            "SDK fabric resource and cloth count");
      check(cloth.global_pose.has_value(), "SDK cloth pose captured");
      near(cloth.global_pose->position, v(row, 6),
           "SDK cloth global translation");
      const auto &q = cloth.global_pose->rotation;
      near(q.x, f(row, 9), "SDK cloth rotation x");
      near(q.y, f(row, 10), "SDK cloth rotation y");
      near(q.z, f(row, 11), "SDK cloth rotation z");
      near(q.w, f(row, 12), "SDK cloth rotation w");
      check(cloth.sleeping == bool(u(row, 13)) &&
                cloth.cloth_flags == u(row, 14) &&
                cloth.actor_flags == u(row, 15) && cloth.name == row[17],
            "SDK cloth metadata");
      check(cloth.triangle_indices.empty() &&
                cloth.triangle_topology_status == d::DataStatus::not_recorded,
            "SDK missing face topology explicit");
      if (u(row, 16)) {
        check(cloth.positions_status == d::DataStatus::available &&
                  cloth.local_positions_status == d::DataStatus::available &&
                  cloth.world_positions.size() == o.vertices.size(),
              "SDK cloth current particles available");
        std::vector<std::uint32_t> pins;
        for (const auto &item : o.vertices) {
          const auto index = item.first;
          const auto &r = item.second;
          near(cloth.local_positions.at(index), v(r, 4),
               "SDK cloth local particle");
          near(cloth.world_positions.at(index), v(r, 7),
               "SDK cloth independent world transform");
          near(cloth.inverse_masses.at(index), f(r, 10), "SDK inverse mass");
          if (f(r, 10) == 0)
            pins.push_back(index);
          ++checked_vertices;
        }
        check(cloth.pinned_indices == pins, "SDK actual pin states");
      } else
        check(cloth.positions_status == d::DataStatus::not_recorded &&
                  cloth.world_positions.empty(),
              "SDK initial cloth buffer not invented");
      check(cloth.constraints_status == d::DataStatus::available &&
                cloth.constraints.size() == o.constraints.size() &&
                cloth.phases.size() == o.phases.size(),
            "SDK fabric constraints count");
      for (const auto &edge : cloth.constraints) {
        const auto &r = o.constraints.at(edge.index);
        check(edge.particle0 == u(r, 4) && edge.particle1 == u(r, 5) &&
                  edge.set_index == u(r, 7) && edge.rest_length.has_value(),
              "SDK exact constraint endpoints/set");
        near(*edge.rest_length, f(r, 6), "SDK exact constraint rest value");
        ++checked_edges;
      }
      for (std::size_t i = 0; i < cloth.phases.size(); ++i) {
        const auto &r = o.phases.at(static_cast<unsigned>(i));
        check(cloth.phases[i].phase_type == u(r, 4) &&
                  cloth.phases[i].set_index == u(r, 5),
              "SDK cloth phases");
      }
    }
    for (const auto &diagnostic : frame.diagnostics)
      check(diagnostic.severity != d::Severity::error,
            "SDK fixture has no malformed data: " + diagnostic.property + " " +
                diagnostic.message);
  };
  unsigned step = 0;
  for (const auto &section : session.sections())
    if (section.name == "initial_deformables" ||
        section.name == "validation_after_fetch")
      compare(step++, session.snapshot_at_event(section.event_count));
  check(step == 9 && samples.size() == 9 && session.frames().size() == 8,
        "SDK initial+8steps exact markers");
  for (std::size_t i = 0; i < session.frames().size(); ++i)
    compare(static_cast<unsigned>(i + 1), session.snapshot_at_frame(i));
  // Reverse seeking must preserve lifetime and sample data, with no
  // forward-state leakage.
  for (std::size_t i = session.frames().size(); i > 0; --i)
    compare(static_cast<unsigned>(i), session.snapshot_at_frame(i - 1));
  std::cout << "SDK deformables: " << checked_particles << " particle rows, "
            << checked_vertices << " cloth vertices, " << checked_edges
            << " constraints across markers/native frames/reverse seek\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 1 || argc == 3,
          "usage: pvd_deformables_tests [capture.pxd2 expected.csv]");
    for (const bool shared : {false, true}) {
      shared_input = shared;
      for (auto order : {ByteOrder::little, ByteOrder::big})
        for (std::uint8_t width : std::array<std::uint8_t, 2>{4, 8})
          valid(order, width);
      malformed();
      if (argc == 3)
        sdk(argv[1], argv[2]);
    }
    std::cout << "Deformables tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
