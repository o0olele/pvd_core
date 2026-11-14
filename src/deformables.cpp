#include "pvd/deformables.hpp"
#include "pvd/render.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace pvd::deformables {
namespace {
struct Issue {
  DiagnosticCode code;
  std::string property, message;
};
ObjectKey key(const InstanceState &o) { return {o.id, o.generation}; }
[[noreturn]] void fail(DiagnosticCode code, const std::string &p,
                       const std::string &message) {
  throw Issue{code, p, message};
}
bool type(const PropertyValue &value, const char *name) {
  return value.incoming_type == QualifiedName{"physx3", name};
}
std::uint64_t u(const PropertyValue &value, std::size_t at, std::size_t width,
                const std::string &p) {
  const auto &b = value.raw_bytes();
  if (at > b.size() || width > b.size() - at)
    fail(DiagnosticCode::malformed_payload, p, "truncated array field");
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < width; ++i)
    v |= std::uint64_t(b[at + i])
         << ((value.byte_order == ByteOrder::little ? i : width - i - 1) * 8);
  return v;
}
float f(const PropertyValue &value, std::size_t at, const std::string &p) {
  const auto bits = static_cast<std::uint32_t>(u(value, at, 4, p));
  float v;
  std::memcpy(&v, &bits, 4);
  if (!std::isfinite(v))
    fail(DiagnosticCode::invalid_value, p, "non-finite floating value");
  return v;
}
Vec3 vec(const PropertyValue &value, std::size_t at, const std::string &p) {
  return {f(value, at, p), f(value, at + 4, p), f(value, at + 8, p)};
}
void bounds(std::optional<Bounds> &box, Vec3 v) {
  if (!box) {
    box = Bounds{v, v};
    return;
  }
  box->minimum = {std::min(box->minimum.x, v.x), std::min(box->minimum.y, v.y),
                  std::min(box->minimum.z, v.z)};
  box->maximum = {std::max(box->maximum.x, v.x), std::max(box->maximum.y, v.y),
                  std::max(box->maximum.z, v.z)};
}
struct Build {
  const Metadata &metadata;
  const Snapshot &state;
  const Options &options;
  Frame result;
  std::size_t used_cloth = 0, used_constraints = 0, used_particles = 0,
              used_owners = 0;
  bool is(const InstanceState &o, const char *name) const {
    return metadata.is_a(o.class_name, {"physx3", name});
  }
  void report(const InstanceState &owner, Severity severity,
              DiagnosticCode code, const std::string &property,
              const std::string &message) {
    if (severity != Severity::info)
      result.complete = false;
    if (result.diagnostics.size() < options.max_diagnostics)
      result.diagnostics.push_back(
          {severity, code, key(owner), property, message});
    else {
      ++result.suppressed_diagnostics;
      result.complete = false;
    }
  }
  void caught(const InstanceState &owner, const Issue &issue) {
    report(owner, Severity::error, issue.code, issue.property, issue.message);
  }
  void add(std::size_t &count, std::uint64_t amount, std::size_t maximum,
           const char *p) {
    if (amount > maximum || count > maximum - static_cast<std::size_t>(amount))
      fail(DiagnosticCode::resource_limit, p,
           "configured total data budget exceeded");
    count += static_cast<std::size_t>(amount);
  }
  const PropertyValue *array(const InstanceState &object, const char *p,
                             const char *datatype, std::size_t stride) {
    const auto *value = object.property(p);
    if (!value)
      return nullptr;
    if (!type(*value, datatype))
      fail(DiagnosticCode::invalid_type, p,
           std::string("expected physx3::") + datatype);
    const auto size = value->raw_bytes().size();
    if (size > options.max_property_bytes)
      fail(DiagnosticCode::resource_limit, p, "property byte budget exceeded");
    if (value->item_count > size / stride || size != value->item_count * stride)
      fail(DiagnosticCode::count_mismatch, p,
           "array byte size and item count disagree");
    return value;
  }
  std::optional<std::uint32_t> integer(const InstanceState &object,
                                       const char *p, bool allow_bool = false) {
    const auto *value = object.property(p);
    if (!value)
      return {};
    if (value->item_count != 1)
      fail(DiagnosticCode::count_mismatch, p, "expected one integer");
    if (!type(*value, "PvdU8") && !type(*value, "PvdU16") &&
        !type(*value, "PvdU32") && !type(*value, "PvdU64") &&
        !(allow_bool && type(*value, "PvdBool")))
      fail(DiagnosticCode::invalid_type, p,
           "expected an unsigned integer wire type");
    const auto width = type(*value, "PvdU64")   ? 8u
                       : type(*value, "PvdU32") ? 4u
                       : type(*value, "PvdU16") ? 2u
                                                : 1u;
    if (value->raw_bytes().size() != width)
      fail(DiagnosticCode::malformed_payload, p,
           "integer byte size differs from its wire type");
    try {
      auto v = value->as_uint();
      if (!v || *v > std::numeric_limits<std::uint32_t>::max())
        fail(DiagnosticCode::invalid_type, p,
             "expected uint32-compatible integer");
      return static_cast<std::uint32_t>(*v);
    } catch (const std::out_of_range &) {
      fail(DiagnosticCode::malformed_payload, p,
           "integer has truncated source bytes");
    }
  }
  std::optional<std::uint64_t> reference(const InstanceState &object,
                                         const char *p) {
    const auto *value = object.property(p);
    if (!value)
      return {};
    if (value->item_count != 1)
      fail(DiagnosticCode::count_mismatch, p, "expected one object reference");
    if (!type(*value, "VoidPtr") ||
        (value->pointer_width != 4 && value->pointer_width != 8) ||
        value->raw_bytes().size() != value->pointer_width ||
        !value->reference_ids || value->reference_ids->size() != 1)
      fail(DiagnosticCode::invalid_type, p,
           "expected one complete resolved VoidPtr reference");
    auto v = value->as_object_ref();
    if (!v)
      fail(DiagnosticCode::invalid_type, p,
           "expected resolved object reference");
    return v;
  }
  std::string name(const InstanceState &object) {
    const auto *p = object.property("Name");
    return p ? p->text.value_or("") : "";
  }
  std::optional<ObjectKey> scene(const InstanceState &owner) {
    const auto id = reference(owner, "Scene");
    const auto *object = id ? state.instance(*id) : nullptr;
    if (!object || !is(*object, "PxScene")) {
      report(owner, Severity::warning, DiagnosticCode::missing_scene, "Scene",
             "native owner has no captured live scene reference");
      return {};
    }
    return key(*object);
  }
  bool selected(const InstanceState &owner,
                std::optional<ObjectKey> scene_key) {
    if (options.scene && (!scene_key || !(*options.scene == *scene_key)))
      return false;
    if (options.owners.empty())
      return true;
    return std::find(options.owners.begin(), options.owners.end(),
                     key(owner)) != options.owners.end();
  }
  void cloth_particles(const InstanceState &owner, ClothSurface &cloth) {
    const auto *pose = owner.property("GlobalPose");
    if (pose) {
      if (!type(*pose, "PxTransform") || pose->item_count != 1 ||
          pose->raw_bytes().size() != 28)
        fail(DiagnosticCode::invalid_type, "GlobalPose",
             "expected one complete PxTransform");
      const auto t = pose->as_transform();
      const auto &q = t->rotation;
      const auto length = double(q.x) * q.x + double(q.y) * q.y +
                          double(q.z) * q.z + double(q.w) * q.w;
      if (!std::isfinite(length) || std::abs(length - 1) > 1e-3 ||
          !std::isfinite(t->position.x) || !std::isfinite(t->position.y) ||
          !std::isfinite(t->position.z))
        fail(DiagnosticCode::invalid_value, "GlobalPose",
             "non-finite or non-unit transform");
      cloth.global_pose = t;
    } else
      report(owner, Severity::warning, DiagnosticCode::data_not_recorded,
             "GlobalPose",
             "cloth world coordinates require captured GlobalPose; no identity "
             "transform is assumed");
    const auto *particles =
        array(owner, "ParticleBuffer", "PxClothParticle", 16);
    if (!particles) {
      report(owner, Severity::warning, DiagnosticCode::data_not_recorded,
             "ParticleBuffer",
             "current cloth particles were not captured (native initial state "
             "may precede the first update)");
      return;
    }
    if (cloth.declared_count && particles->item_count != *cloth.declared_count)
      fail(DiagnosticCode::count_mismatch, "ParticleBuffer",
           "current particle count differs from NbParticles");
    if (particles->item_count > std::numeric_limits<std::uint32_t>::max())
      fail(DiagnosticCode::resource_limit, "ParticleBuffer",
           "native cloth particle indices are uint32");
    add(used_cloth, particles->item_count, options.max_cloth_vertices,
        "ParticleBuffer");
    std::vector<Vec3> local, world;
    std::vector<float> masses;
    std::vector<std::uint32_t> pinned;
    std::optional<Bounds> box;
    local.reserve(static_cast<std::size_t>(particles->item_count));
    masses.reserve(local.capacity());
    if (cloth.global_pose)
      world.reserve(local.capacity());
    for (std::size_t i = 0; i < particles->item_count; ++i) {
      const auto position = vec(*particles, i * 16, "ParticleBuffer");
      const auto mass = f(*particles, i * 16 + 12, "ParticleBuffer");
      if (mass < 0)
        fail(DiagnosticCode::invalid_value, "ParticleBuffer",
             "negative inverse particle mass");
      local.push_back(position);
      masses.push_back(mass);
      if (mass == 0)
        pinned.push_back(static_cast<std::uint32_t>(i));
      if (cloth.global_pose) {
        const auto point =
            render::transform_point(*cloth.global_pose, position);
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z))
          fail(DiagnosticCode::invalid_value, "GlobalPose",
               "world coordinate overflow");
        world.push_back(point);
        bounds(box, point);
      }
    }
    cloth.local_positions = std::move(local);
    cloth.world_positions = std::move(world);
    cloth.inverse_masses = std::move(masses);
    cloth.pinned_indices = std::move(pinned);
    cloth.world_bounds = box;
    cloth.local_positions_status = DataStatus::available;
    cloth.positions_status =
        cloth.global_pose ? DataStatus::available : DataStatus::not_recorded;
  }
  void cloth_constraints(const InstanceState &owner, ClothSurface &cloth) {
    const auto id = reference(owner, "Fabric");
    const auto *fabric = id ? state.instance(*id) : nullptr;
    if (!fabric || !is(*fabric, "PxClothFabric"))
      fail(DiagnosticCode::dangling_reference, "Fabric",
           "cloth fabric resource is absent or has the wrong class");
    cloth.fabric = key(*fabric);
    const auto count = integer(*fabric, "NbParticles");
    if (count && cloth.declared_count && *count != *cloth.declared_count)
      fail(DiagnosticCode::count_mismatch, "Fabric",
           "fabric and cloth particle counts disagree");
    const auto vertex_count = count ? count : cloth.declared_count;
    if (!vertex_count)
      fail(DiagnosticCode::missing_property, "NbParticles",
           "fabric constraint indices require a known particle count");
    const auto *pairs = array(*fabric, "ParticleIndices", "PvdU32", 4);
    if (!pairs) {
      report(owner, Severity::warning, DiagnosticCode::data_not_recorded,
             "ParticleIndices", "fabric constraints were not captured");
      return;
    }
    if (pairs->item_count % 2)
      fail(DiagnosticCode::count_mismatch, "ParticleIndices",
           "fabric constraints require index pairs");
    const auto n = pairs->item_count / 2;
    if (n > std::numeric_limits<std::uint32_t>::max())
      fail(DiagnosticCode::resource_limit, "ParticleIndices",
           "native fabric constraint indices are uint32");
    add(used_constraints, n, options.max_constraints, "ParticleIndices");
    const auto *rest = array(*fabric, "Restvalues", "PvdF32", 4);
    if (rest && rest->item_count != n)
      fail(DiagnosticCode::count_mismatch, "Restvalues",
           "one rest value is required per constraint");
    const auto *sets = array(*fabric, "Sets", "PvdU32", 4);
    const auto *phases = array(*fabric, "Phases", "PxClothFabricPhase", 8);
    if (!sets)
      report(owner, Severity::info, DiagnosticCode::data_not_recorded, "Sets",
             "fabric constraint-set membership was not captured");
    if (!phases)
      report(owner, Severity::info, DiagnosticCode::data_not_recorded, "Phases",
             "fabric solver phases were not captured");
    std::vector<std::uint32_t> endpoints;
    if (sets) {
      if (sets->item_count > n + 1)
        fail(DiagnosticCode::count_mismatch, "Sets",
             "more fabric sets than constraints");
      std::uint32_t previous = 0;
      for (std::size_t i = 0; i < sets->item_count; ++i) {
        const auto end = static_cast<std::uint32_t>(u(*sets, i * 4, 4, "Sets"));
        if (end < previous || end > n)
          fail(DiagnosticCode::invalid_index, "Sets",
               "set prefix is not monotonic or exceeds constraints");
        endpoints.push_back(end);
        previous = end;
      }
      if (!endpoints.empty() && endpoints.back() != n)
        fail(DiagnosticCode::count_mismatch, "Sets",
             "final set does not cover every constraint");
    }
    std::vector<ClothPhase> decoded_phases;
    if (phases) {
      if (phases->item_count > options.max_constraints)
        fail(DiagnosticCode::resource_limit, "Phases",
             "phase count exceeds configured limit");
      for (std::size_t i = 0; i < phases->item_count; ++i) {
        const auto phase = static_cast<std::uint32_t>(
                       u(*phases, i * 8, 4, "Phases")),
                   set = static_cast<std::uint32_t>(
                       u(*phases, i * 8 + 4, 4, "Phases"));
        if (phase < 1 || phase > 4 || !sets || set >= endpoints.size())
          fail(DiagnosticCode::invalid_value, "Phases",
               "unknown phase type or invalid set index");
        decoded_phases.push_back({phase, set});
      }
    }
    std::vector<ClothConstraint> constraints;
    constraints.reserve(static_cast<std::size_t>(n));
    std::size_t set_index = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const auto a = static_cast<std::uint32_t>(
                     u(*pairs, i * 8, 4, "ParticleIndices")),
                 b = static_cast<std::uint32_t>(
                     u(*pairs, i * 8 + 4, 4, "ParticleIndices"));
      if (a >= *vertex_count || b >= *vertex_count)
        fail(DiagnosticCode::invalid_index, "ParticleIndices",
             "constraint references a particle outside the fabric");
      while (set_index < endpoints.size() && i >= endpoints[set_index])
        ++set_index;
      ClothConstraint value;
      value.index = static_cast<std::uint32_t>(i);
      value.particle0 = a;
      value.particle1 = b;
      if (rest) {
        const auto r = f(*rest, i * 4, "Restvalues");
        if (r < 0)
          fail(DiagnosticCode::invalid_value, "Restvalues",
               "negative distance constraint rest value");
        value.rest_length = r;
      }
      if (set_index < endpoints.size())
        value.set_index = static_cast<std::uint32_t>(set_index);
      constraints.push_back(value);
    }
    cloth.constraints = std::move(constraints);
    cloth.phases = std::move(decoded_phases);
    cloth.constraints_status = DataStatus::available;
    if (!rest)
      report(
          owner, Severity::info, DiagnosticCode::data_not_recorded,
          "Restvalues",
          "fabric edges are available but their rest values were not captured");
  }
  void cloth(const InstanceState &owner, ObjectKey scene_key) {
    ClothSurface value;
    value.key = {scene_key, key(owner)};
    value.name = name(owner);
    value.pickable = owner.pickable;
    value.color = owner.color;
    try {
      value.actor_flags = integer(owner, "ActorFlags");
      value.cloth_flags = integer(owner, "ClothFlags");
      value.declared_count = integer(owner, "NbParticles");
      const auto sleep = integer(owner, "IsSleeping", true);
      if (sleep && *sleep > 1)
        fail(DiagnosticCode::invalid_value, "IsSleeping",
             "boolean must be zero or one");
      if (sleep)
        value.sleeping = *sleep != 0;
      cloth_particles(owner, value);
    } catch (const Issue &issue) {
      value.positions_status = value.local_positions_status =
          DataStatus::malformed;
      caught(owner, issue);
    }
    try {
      cloth_constraints(owner, value);
    } catch (const Issue &issue) {
      value.constraints_status = DataStatus::malformed;
      caught(owner, issue);
    }
    report(owner, Severity::info, DiagnosticCode::topology_not_recorded,
           "Fabric",
           "native PhysX 3.4 fabric records solver constraints, not original "
           "surface triangles/quads; displaying exact particles/constraint "
           "edges only");
    result.cloths.push_back(std::move(value));
  }
  DataStatus missing_buffer(const InstanceState &owner,
                            const ParticleCloud &cloud, const char *property,
                            std::uint32_t flag, bool required) {
    const bool disabled = cloud.read_flags && ((*cloud.read_flags & flag) == 0);
    report(owner, required ? Severity::warning : Severity::info,
           disabled ? DiagnosticCode::read_flag_disabled
                    : DiagnosticCode::data_not_recorded,
           property,
           disabled ? "buffer absent and application read flag is disabled "
                      "(native CPU frame updates may still stream it later)"
                    : "particle buffer was not captured");
    return disabled ? DataStatus::disabled : DataStatus::not_recorded;
  }
  void indices(const InstanceState &owner, ParticleCloud &cloud) {
    cloud.declared_count = integer(owner, "NbParticles");
    cloud.valid_range = integer(owner, "ValidParticleRange");
    cloud.max_particles = integer(owner, "MaxParticles");
    if (!cloud.declared_count || !cloud.valid_range) {
      report(owner, Severity::warning, DiagnosticCode::data_not_recorded,
             "NbParticles/ValidParticleRange",
             "particle validity metadata was not captured");
      return;
    }
    const auto count = *cloud.declared_count, range = *cloud.valid_range;
    if (count > range || (!count && range) ||
        (cloud.max_particles && range > *cloud.max_particles))
      fail(DiagnosticCode::count_mismatch, "ValidParticleRange",
           "valid particle count/range exceeds source capacity");
    if (range > options.max_valid_range)
      fail(DiagnosticCode::resource_limit, "ValidParticleRange",
           "particle sparse index range exceeds configured limit");
    add(used_particles, count, options.max_particles, "NbParticles");
    if (!count) {
      cloud.indices_status = DataStatus::available;
      return;
    }
    const auto *bitmap = array(owner, "ValidParticleBitmap", "PvdU32", 4);
    if (!bitmap)
      fail(DiagnosticCode::missing_property, "ValidParticleBitmap",
           "nonempty particle range needs its validity bitmap");
    const auto words = (std::uint64_t(range) + 31) / 32,
               sdk_words = (std::uint64_t(range) >> 5) + 1;
    if (bitmap->item_count > options.max_bitmap_words)
      fail(DiagnosticCode::resource_limit, "ValidParticleBitmap",
           "bitmap word count exceeds limit");
    if (bitmap->item_count != words && bitmap->item_count != sdk_words)
      fail(DiagnosticCode::count_mismatch, "ValidParticleBitmap",
           "bitmap has neither minimal nor SDK padded word count");
    std::vector<Particle> particles;
    particles.reserve(count);
    bool padding = false;
    for (std::size_t word = 0; word < bitmap->item_count; ++word) {
      const auto bits = static_cast<std::uint32_t>(
          u(*bitmap, word * 4, 4, "ValidParticleBitmap"));
      for (unsigned bit = 0; bit < 32; ++bit)
        if (bits & (std::uint32_t(1) << bit)) {
          const auto index = std::uint64_t(word) * 32 + bit;
          if (index >= range) {
            padding = true;
            continue;
          }
          if (particles.size() >= count)
            fail(DiagnosticCode::count_mismatch, "ValidParticleBitmap",
                 "bitmap has more valid particles than NbParticles");
          Particle p;
          p.index = static_cast<std::uint32_t>(index);
          particles.push_back(p);
        }
    }
    if (particles.size() != count)
      fail(DiagnosticCode::count_mismatch, "ValidParticleBitmap",
           "bitmap population differs from NbParticles");
    if (padding)
      report(owner, Severity::warning, DiagnosticCode::padding_bits,
             "ValidParticleBitmap",
             "set bitmap padding bits outside ValidParticleRange were ignored, "
             "never turned into particles");
    cloud.particles = std::move(particles);
    cloud.indices_status = DataStatus::available;
  }
  void vector_buffer(const InstanceState &owner, ParticleCloud &cloud,
                     const char *name, std::uint32_t flag, DataStatus &status,
                     bool position) {
    if (cloud.particles.empty()) {
      status = DataStatus::available;
      return;
    }
    const auto *data = array(owner, name, "PxVec3", 12);
    if (!data) {
      status = missing_buffer(owner, cloud, name, flag, position);
      return;
    }
    if (data->item_count != cloud.particles.size())
      fail(DiagnosticCode::count_mismatch, name,
           "PVD packed vector buffer must contain exactly NbParticles rows, "
           "not ValidParticleRange rows");
    std::vector<Vec3> points;
    points.reserve(cloud.particles.size());
    for (std::size_t i = 0; i < cloud.particles.size(); ++i)
      points.push_back(vec(*data, i * 12, name));
    for (std::size_t i = 0; i < points.size(); ++i) {
      if (position) {
        cloud.particles[i].position = points[i];
        bounds(cloud.world_bounds, points[i]);
      } else
        cloud.particles[i].velocity = points[i];
    }
    status = DataStatus::available;
  }
  void flag_buffer(const InstanceState &owner, ParticleCloud &cloud) {
    if (cloud.particles.empty()) {
      cloud.flags_status = DataStatus::available;
      return;
    }
    const auto *data = array(owner, "Flags", "PvdU16", 2);
    if (!data) {
      cloud.flags_status = missing_buffer(owner, cloud, "Flags", 8, false);
      return;
    }
    if (data->item_count != cloud.particles.size())
      fail(DiagnosticCode::count_mismatch, "Flags",
           "packed flags must match valid particle count");
    bool unknown = false;
    for (std::size_t i = 0; i < cloud.particles.size(); ++i) {
      const auto bits = static_cast<std::uint32_t>(u(*data, i * 2, 2, "Flags"));
      if (!(bits & 1))
        fail(
            DiagnosticCode::invalid_value, "Flags",
            "validity bitmap row is missing eVALID in captured particle flags");
      unknown = unknown || ((bits & ~31u) != 0);
      cloud.particles[i].flags = bits;
    }
    if (unknown)
      report(owner, Severity::warning, DiagnosticCode::invalid_value, "Flags",
             "unknown particle flag bits were preserved");
    cloud.flags_status = DataStatus::available;
  }
  void scalar_buffer(const InstanceState &owner, ParticleCloud &cloud,
                     const char *name, std::uint32_t flag, bool density) {
    auto &status = density ? cloud.densities_status : cloud.rest_offsets_status;
    if (cloud.particles.empty()) {
      status = DataStatus::available;
      return;
    }
    const auto *data = array(owner, name, "PvdF32", 4);
    if (!data) {
      status = missing_buffer(owner, cloud, name, flag, false);
      return;
    }
    if (data->item_count != cloud.particles.size())
      fail(DiagnosticCode::count_mismatch, name,
           "packed scalar buffer must match valid particle count");
    std::vector<float> values;
    for (std::size_t i = 0; i < cloud.particles.size(); ++i) {
      const auto value = f(*data, i * 4, name);
      if (value < 0)
        fail(DiagnosticCode::invalid_value, name,
             "negative particle density or rest offset");
      values.push_back(value);
    }
    for (std::size_t i = 0; i < values.size(); ++i)
      if (density)
        cloud.particles[i].density = values[i];
      else
        cloud.particles[i].rest_offset = values[i];
    status = DataStatus::available;
  }
  void normals(const InstanceState &owner, ParticleCloud &cloud) {
    if (cloud.particles.empty()) {
      cloud.collision_normals_status = DataStatus::available;
      return;
    }
    const auto *data = array(owner, "CollisionNormals", "PxVec3", 12);
    if (!data) {
      cloud.collision_normals_status =
          missing_buffer(owner, cloud, "CollisionNormals", 16, false);
      return;
    }
    if (data->item_count != cloud.particles.size())
      fail(DiagnosticCode::count_mismatch, "CollisionNormals",
           "packed normals must match valid particle count");
    if (cloud.flags_status != DataStatus::available) {
      report(owner, Severity::info, DiagnosticCode::data_not_recorded,
             "CollisionNormals",
             "collision normal validity requires captured particle collision "
             "flags");
      return;
    }
    for (std::size_t i = 0; i < cloud.particles.size(); ++i)
      if (cloud.particles[i].flags && (*cloud.particles[i].flags & 14u))
        cloud.particles[i].collision_normal =
            vec(*data, i * 12, "CollisionNormals");
    cloud.collision_normals_status = DataStatus::available;
  }
  void particles(const InstanceState &owner, ObjectKey scene_key) {
    ParticleCloud cloud;
    cloud.key = {scene_key, key(owner)};
    cloud.name = name(owner);
    cloud.kind = is(owner, "PxParticleFluid") ? ParticleKind::fluid
                                              : ParticleKind::system;
    cloud.pickable = owner.pickable;
    cloud.color = owner.color;
    try {
      cloud.actor_flags = integer(owner, "ActorFlags");
      cloud.base_flags = integer(owner, "ParticleBaseFlags");
      cloud.read_flags = integer(owner, "ParticleReadDataFlags");
      cloud.updates_disabled =
          cloud.base_flags && ((*cloud.base_flags & 4u) == 0);
      if (cloud.updates_disabled)
        report(owner, Severity::warning, DiagnosticCode::read_flag_disabled,
               "ParticleBaseFlags",
               "eENABLED is clear; native PVD skips frame updates and stored "
               "arrays may be the last captured state");
      indices(owner, cloud);
    } catch (const Issue &issue) {
      cloud.indices_status = DataStatus::malformed;
      caught(owner, issue);
    }
    if (cloud.indices_status == DataStatus::available) {
      try {
        vector_buffer(owner, cloud, "Positions", 1, cloud.positions_status,
                      true);
      } catch (const Issue &issue) {
        cloud.positions_status = DataStatus::malformed;
        caught(owner, issue);
      }
      try {
        vector_buffer(owner, cloud, "Velocities", 2, cloud.velocities_status,
                      false);
      } catch (const Issue &issue) {
        cloud.velocities_status = DataStatus::malformed;
        caught(owner, issue);
      }
      try {
        flag_buffer(owner, cloud);
      } catch (const Issue &issue) {
        for (auto &particle : cloud.particles)
          particle.flags.reset();
        cloud.flags_status = DataStatus::malformed;
        caught(owner, issue);
      }
      try {
        scalar_buffer(owner, cloud, "RestOffsets", 4, false);
      } catch (const Issue &issue) {
        cloud.rest_offsets_status = DataStatus::malformed;
        caught(owner, issue);
      }
      if (cloud.kind == ParticleKind::fluid)
        try {
          scalar_buffer(owner, cloud, "Densities", 64, true);
        } catch (const Issue &issue) {
          cloud.densities_status = DataStatus::malformed;
          caught(owner, issue);
        }
      try {
        normals(owner, cloud);
      } catch (const Issue &issue) {
        for (auto &particle : cloud.particles)
          particle.collision_normal.reset();
        cloud.collision_normals_status = DataStatus::malformed;
        caught(owner, issue);
      }
    }
    result.particle_clouds.push_back(std::move(cloud));
  }
  Frame run() {
    result.event_count = state.event_count;
    const auto validate_selection = [&](ObjectKey selected_key,
                                        bool scene_key) {
      const auto *object = state.instance(selected_key.id);
      if (object && key(*object) == selected_key &&
          (scene_key
               ? is(*object, "PxScene")
               : (is(*object, "PxCloth") || is(*object, "PxParticleSystem") ||
                  is(*object, "PxParticleFluid"))))
        return true;
      InstanceState requested;
      requested.id = selected_key.id;
      requested.generation = selected_key.generation;
      report(requested, Severity::error, DiagnosticCode::dangling_reference,
             scene_key ? "selected scene" : "selected owner",
             "selection does not identify a live native object of the "
             "requested generation/class");
      return false;
    };
    if (options.scene && !validate_selection(*options.scene, true))
      return std::move(result);
    for (const auto selected_key : options.owners)
      validate_selection(selected_key, false);
    for (const auto &entry : state.instances) {
      const auto &owner = entry.second;
      const bool cloth_owner = is(owner, "PxCloth"),
                 particle_owner = is(owner, "PxParticleSystem") ||
                                  is(owner, "PxParticleFluid");
      if (!cloth_owner && !particle_owner)
        continue;
      if (!options.owners.empty() &&
          std::find(options.owners.begin(), options.owners.end(), key(owner)) ==
              options.owners.end())
        continue;
      try {
        const auto owner_scene = scene(owner);
        if (!selected(owner, owner_scene))
          continue;
        if (used_owners >= options.max_owners)
          fail(DiagnosticCode::resource_limit, "owner",
               "native deformable owner limit exceeded");
        ++used_owners;
        if (cloth_owner)
          cloth(owner, owner_scene.value_or(ObjectKey{}));
        else
          particles(owner, owner_scene.value_or(ObjectKey{}));
      } catch (const Issue &issue) {
        caught(owner, issue);
      }
    }
    return std::move(result);
  }
};
} // namespace
Frame extract(const Metadata &metadata, const Snapshot &state,
              const Options &options) {
  if (!options.max_owners || !options.max_cloth_vertices ||
      !options.max_constraints || !options.max_particles ||
      !options.max_valid_range || !options.max_bitmap_words ||
      !options.max_property_bytes || !options.max_diagnostics)
    throw std::invalid_argument(
        "deformable extraction limits must be positive");
  Build build{metadata, state, options, {}};
  return build.run();
}
} // namespace pvd::deformables
