#pragma once
#include <cstddef>

namespace pvd::viewer {
enum class NumberFormat { general, scientific, fixed };
struct InspectorPreferences {
  NumberFormat number_format = NumberFormat::general;
  unsigned float_precision = 9, double_precision = 17;
  std::size_t array_page_size = 32;
};
inline bool
valid_inspector_preferences(const InspectorPreferences &v) noexcept {
  return (v.number_format == NumberFormat::general ||
          v.number_format == NumberFormat::scientific ||
          v.number_format == NumberFormat::fixed) &&
         v.float_precision >= 1 && v.float_precision <= 17 &&
         v.double_precision >= 1 && v.double_precision <= 17 &&
         (v.array_page_size == 32 || v.array_page_size == 128 ||
          v.array_page_size == 512 || v.array_page_size == 2048);
}
} // namespace pvd::viewer
