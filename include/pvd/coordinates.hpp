#pragma once
#include "pvd/session.hpp"

namespace pvd::coordinates {
// PhysX 3.4 OriginShift affects retained world-space values even when the SDK
// never resends their properties. Session keeps a bounded immutable shadow for
// the confirmed native fields. raw_bytes()/as_* and source events stay exact.
// These explicit readers select that shadow, or the original bytes otherwise.
PVD_CORE_API const Bytes &native_bytes(const PropertyValue &) noexcept;
PVD_CORE_API PropertyValue native_value(const PropertyValue &);
PVD_CORE_API std::optional<Transform> native_transform(const PropertyValue &,
                                                       std::size_t item = 0);
PVD_CORE_API std::optional<Vec3> native_vec3(const PropertyValue &,
                                             std::size_t item = 0);
} // namespace pvd::coordinates
