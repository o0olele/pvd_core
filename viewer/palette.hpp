#pragma once
#include "pvd/deformables.hpp"
#include "pvd/render.hpp"
#include <array>

namespace pvd::viewer {
using PaletteRgba = std::array<float, 4>;
struct ColorPalette {
  PaletteRgba static_actor{220.f / 255.f, 100.f / 255.f, 100.f / 255.f, 1};
  PaletteRgba sleeping{230.f / 255.f, 170.f / 255.f, 70.f / 255.f, 1};
  PaletteRgba active{95.f / 255.f, 205.f / 255.f, 125.f / 255.f, 1};
  PaletteRgba kinematic{70.f / 255.f, 135.f / 255.f, 245.f / 255.f, 1};
  PaletteRgba trigger{190.f / 255.f, 100.f / 255.f, 220.f / 255.f, 1};
  PaletteRgba articulation{65.f / 255.f, 195.f / 255.f, 200.f / 255.f, 1};
  PaletteRgba cloth{.43f, .69f, .82f, 1};
  PaletteRgba particle{.91f, .72f, .32f, 1};
  PaletteRgba particle_fluid{.26f, .68f, .97f, 1};
  PaletteRgba profile_bar{.68f, .34f, .34f, 1};
  // Default profile bars retain the existing event-ID HSV scheme. A custom
  // uniform bar color is explicit, rather than silently changing the default.
  bool profile_by_event = true;
};
enum class PaletteRole {
  static_actor,
  sleeping,
  active,
  kinematic,
  trigger,
  articulation,
  cloth,
  particle,
  particle_fluid,
  profile_bar
};
inline constexpr std::array<PaletteRole, 10> color_palette_roles{
    PaletteRole::static_actor,   PaletteRole::sleeping,
    PaletteRole::active,         PaletteRole::kinematic,
    PaletteRole::trigger,        PaletteRole::articulation,
    PaletteRole::cloth,          PaletteRole::particle,
    PaletteRole::particle_fluid, PaletteRole::profile_bar};
const char *palette_role_name(PaletteRole) noexcept;
PaletteRgba &palette_color(ColorPalette &, PaletteRole);
const PaletteRgba &palette_color(const ColorPalette &, PaletteRole);
ColorPalette default_color_palette() noexcept;
bool valid_palette_color(const PaletteRgba &) noexcept;
bool valid_color_palette(const ColorPalette &) noexcept;
PaletteRgba palette_rgba(Color) noexcept;
Color palette_byte_color(const PaletteRgba &); // Rejects NaN/out-of-domain.
// Recorded/status keeps recorded object color first, then the original
// trigger > kinematic > static > sleeping > active status precedence.
Color status_color(const render::ShapeInstance &, const ColorPalette &);
// Explicit Actor-kind mode overrides recorded/status, matching existing modes.
Color actor_kind_color(const render::ShapeInstance &, const ColorPalette &);
PaletteRgba cloth_color(const deformables::ClothSurface &,
                        const ColorPalette &);
PaletteRgba particle_color(const deformables::ParticleCloud &,
                           const ColorPalette &);
PaletteRgba profile_bar_color(std::uint16_t event_id, const ColorPalette &);
struct PaletteLegendEntry {
  const char *label;
  Color color;
};
std::array<PaletteLegendEntry, 5> status_palette_legend(const ColorPalette &);
std::array<PaletteLegendEntry, 4>
actor_kind_palette_legend(const ColorPalette &);
} // namespace pvd::viewer
