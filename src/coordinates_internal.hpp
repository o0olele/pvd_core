#pragma once
#include "pvd/coordinates.hpp"

namespace pvd::coordinates::detail {
bool coordinate_field(const Metadata &, const InstanceState &,
                      const std::string &);
bool query_publication(const Metadata &, const InstanceState &,
                       const std::string &);
bool epoch_field(const Metadata &, const InstanceState &, const std::string &);
// Called at an OriginShift in replay order. No inference for custom classes,
// local shape/cloth coordinates, velocities, or scene-less named cameras.
void apply_shift(const Metadata &, Snapshot &, std::uint64_t scene_id, Vec3);
} // namespace pvd::coordinates::detail
