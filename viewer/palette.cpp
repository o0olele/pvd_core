#include "palette.hpp"
#include <cmath>
#include <stdexcept>

namespace pvd::viewer {
namespace {
template <class Palette> auto &entry(Palette &p, PaletteRole role) {
  switch (role) {
  case PaletteRole::static_actor:
    return p.static_actor;
  case PaletteRole::sleeping:
    return p.sleeping;
  case PaletteRole::active:
    return p.active;
  case PaletteRole::kinematic:
    return p.kinematic;
  case PaletteRole::trigger:
    return p.trigger;
  case PaletteRole::articulation:
    return p.articulation;
  case PaletteRole::cloth:
    return p.cloth;
  case PaletteRole::particle:
    return p.particle;
  case PaletteRole::particle_fluid:
    return p.particle_fluid;
  case PaletteRole::profile_bar:
    return p.profile_bar;
  }
  throw std::invalid_argument("Unknown palette role");
}
PaletteRgba checked(const PaletteRgba &p) {
  if (!valid_palette_color(p))
    throw std::invalid_argument(
        "Palette RGBA components must be finite in [0,1]");
  return p;
}
} // namespace
const char *palette_role_name(PaletteRole role) noexcept {
  switch (role) {
  case PaletteRole::static_actor:
    return "Static";
  case PaletteRole::sleeping:
    return "Sleeping";
  case PaletteRole::active:
    return "Active dynamic";
  case PaletteRole::kinematic:
    return "Kinematic";
  case PaletteRole::trigger:
    return "Trigger";
  case PaletteRole::articulation:
    return "Articulation (Actor kind)";
  case PaletteRole::cloth:
    return "Cloth";
  case PaletteRole::particle:
    return "Particle system";
  case PaletteRole::particle_fluid:
    return "Particle fluid";
  case PaletteRole::profile_bar:
    return "Profile bar";
  }
  return "Unknown palette role";
}
PaletteRgba &palette_color(ColorPalette &p, PaletteRole role) {
  return entry(p, role);
}
const PaletteRgba &palette_color(const ColorPalette &p, PaletteRole role) {
  return entry(p, role);
}
ColorPalette default_color_palette() noexcept { return {}; }
bool valid_palette_color(const PaletteRgba &p) noexcept {
  for (auto c : p)
    if (!std::isfinite(c) || c < 0 || c > 1)
      return false;
  return true;
}
bool valid_color_palette(const ColorPalette &p) noexcept {
  for (auto role : color_palette_roles)
    if (!valid_palette_color(palette_color(p, role)))
      return false;
  return true;
}
PaletteRgba palette_rgba(Color c) noexcept {
  return {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
}
Color palette_byte_color(const PaletteRgba &p) {
  checked(p);
  const auto byte = [](float v) {
    return static_cast<std::uint8_t>(v * 255.f + .5f);
  };
  return {byte(p[0]), byte(p[1]), byte(p[2]), byte(p[3])};
}
Color status_color(const render::ShapeInstance &s, const ColorPalette &p) {
  if (s.color)
    return *s.color;
  if (s.flags.trigger)
    return palette_byte_color(p.trigger);
  if (s.actor.kinematic)
    return palette_byte_color(p.kinematic);
  if (s.actor.kind == render::ActorKind::rigid_static)
    return palette_byte_color(p.static_actor);
  return palette_byte_color(s.actor.sleeping == true ? p.sleeping : p.active);
}
Color actor_kind_color(const render::ShapeInstance &s, const ColorPalette &p) {
  if (s.actor.kinematic)
    return palette_byte_color(p.kinematic);
  if (s.actor.kind == render::ActorKind::rigid_static)
    return palette_byte_color(p.static_actor);
  return palette_byte_color(s.actor.kind == render::ActorKind::articulation_link
                                ? p.articulation
                                : p.active);
}
PaletteRgba cloth_color(const deformables::ClothSurface &s,
                        const ColorPalette &p) {
  return s.color ? palette_rgba(*s.color) : checked(p.cloth);
}
PaletteRgba particle_color(const deformables::ParticleCloud &s,
                           const ColorPalette &p) {
  return s.color ? palette_rgba(*s.color)
                 : checked(s.kind == deformables::ParticleKind::fluid
                               ? p.particle_fluid
                               : p.particle);
}
PaletteRgba profile_bar_color(std::uint16_t event_id, const ColorPalette &p) {
  if (!p.profile_by_event)
    return checked(p.profile_bar);
  float h = float((std::uint32_t(event_id) * 37u) % 255u) / 255.f;
  h = std::fmod(h, 1.f) / (60.f / 360.f);
  const auto sector = int(h);
  const auto f = h - float(sector);
  constexpr float value = .68f, saturation = .50f;
  const auto low = value * (1.f - saturation),
             fall = value * (1.f - saturation * f),
             rise = value * (1.f - saturation * (1.f - f));
  switch (sector) {
  case 0:
    return {value, rise, low, 1};
  case 1:
    return {fall, value, low, 1};
  case 2:
    return {low, value, rise, 1};
  case 3:
    return {low, fall, value, 1};
  case 4:
    return {rise, low, value, 1};
  default:
    return {value, low, fall, 1};
  }
}
std::array<PaletteLegendEntry, 5> status_palette_legend(const ColorPalette &p) {
  return {{{"Trigger", palette_byte_color(p.trigger)},
           {"Kinematic", palette_byte_color(p.kinematic)},
           {"Static", palette_byte_color(p.static_actor)},
           {"Sleeping", palette_byte_color(p.sleeping)},
           {"Dynamic", palette_byte_color(p.active)}}};
}
std::array<PaletteLegendEntry, 4>
actor_kind_palette_legend(const ColorPalette &p) {
  return {{{"Static", palette_byte_color(p.static_actor)},
           {"Dynamic", palette_byte_color(p.active)},
           {"Kinematic", palette_byte_color(p.kinematic)},
           {"Articulation link", palette_byte_color(p.articulation)}}};
}
} // namespace pvd::viewer
