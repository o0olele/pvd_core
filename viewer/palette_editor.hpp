#pragma once
#include "palette.hpp"
#include <string>

namespace pvd::viewer {
struct PaletteControlRect {
  float x = 0, y = 0, width = 0, height = 0;
};
struct PaletteEditResult {
  bool changed = false, restored = false;
  std::array<PaletteControlRect, 10> colors{};
  PaletteControlRect restore;
  std::string error;
};
// Content only. Edits are committed atomically after finite [0,1] validation.
// Restoring object colors preserves the separate profiler bar preference.
PaletteEditResult draw_object_palette_editor(ColorPalette &);
} // namespace pvd::viewer
