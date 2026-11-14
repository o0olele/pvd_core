#include "palette.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
bool same(Color a, Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
Color legacy_status(const render::ShapeInstance &s) {
  if (s.color)
    return *s.color;
  if (s.flags.trigger)
    return {190, 100, 220, 255};
  if (s.actor.kinematic)
    return {70, 135, 245, 255};
  if (s.actor.kind == render::ActorKind::rigid_static)
    return {220, 100, 100, 255};
  if (s.actor.sleeping == true)
    return {230, 170, 70, 255};
  return {95, 205, 125, 255};
}
void defaults() {
  const auto p = default_color_palette();
  check(valid_color_palette(p), "default palette invalid");
  for (const auto kind :
       {render::ActorKind::rigid_static, render::ActorKind::rigid_dynamic,
        render::ActorKind::articulation_link})
    for (bool trigger : {false, true})
      for (bool kinematic : {false, true})
        for (int sleep : {-1, 0, 1})
          for (bool recorded : {false, true}) {
            render::ShapeInstance s;
            s.actor.kind = kind;
            s.flags.trigger = trigger;
            s.actor.kinematic = kinematic;
            if (sleep >= 0)
              s.actor.sleeping = sleep != 0;
            if (recorded)
              s.color = Color{1, 2, 3, 4};
            check(same(status_color(s, p), legacy_status(s)),
                  "default status behavior changed");
            const auto expected = kinematic ? Color{70, 135, 245, 255}
                                  : kind == render::ActorKind::rigid_static
                                      ? Color{220, 100, 100, 255}
                                  : kind == render::ActorKind::articulation_link
                                      ? Color{65, 195, 200, 255}
                                      : Color{95, 205, 125, 255};
            check(same(actor_kind_color(s, p), expected),
                  "default actor-kind behavior changed");
          }
  deformables::ClothSurface cloth;
  check(cloth_color(cloth, p) == PaletteRgba{.43f, .69f, .82f, 1},
        "cloth float default quantized/changed");
  deformables::ParticleCloud cloud;
  cloud.kind = deformables::ParticleKind::system;
  check(particle_color(cloud, p) == PaletteRgba{.91f, .72f, .32f, 1},
        "particle system default changed");
  cloud.kind = deformables::ParticleKind::fluid;
  check(particle_color(cloud, p) == PaletteRgba{.26f, .68f, .97f, 1},
        "particle fluid default changed");
  cloth.color = Color{12, 34, 56, 78};
  cloud.color = cloth.color;
  check(cloth_color(cloth, p) == palette_rgba(*cloth.color) &&
            particle_color(cloud, p) == palette_rgba(*cloud.color),
        "recorded native colors lost priority");
  for (unsigned i = 0; i < 256; ++i) {
    const Color c{std::uint8_t(i), std::uint8_t(255 - i), std::uint8_t(i / 2),
                  std::uint8_t(i)};
    check(same(palette_byte_color(palette_rgba(c)), c),
          "RGBA8 roundtrip changed default channels");
  }
}
void custom() {
  auto p = default_color_palette();
  std::size_t i = 0;
  for (auto role : color_palette_roles)
    palette_color(p, role) = {float(++i) * .05f, .25f, .5f, .75f};
  check(valid_color_palette(p), "valid custom palette rejected");
  render::ShapeInstance s;
  s.actor.kind = render::ActorKind::rigid_dynamic;
  check(same(status_color(s, p), palette_byte_color(p.active)),
        "active custom color ignored");
  s.actor.sleeping = true;
  check(same(status_color(s, p), palette_byte_color(p.sleeping)),
        "sleep color ignored");
  s.actor.kinematic = true;
  check(same(status_color(s, p), palette_byte_color(p.kinematic)),
        "kinematic precedence");
  s.flags.trigger = true;
  check(same(status_color(s, p), palette_byte_color(p.trigger)),
        "trigger precedence");
  s.color = Color{1, 2, 3, 4};
  const auto before = *s.color;
  check(same(status_color(s, p), before) && same(*s.color, before),
        "custom palette mutated/overrode recorded color");
  p.profile_by_event = false;
  check(profile_bar_color(1, p) == p.profile_bar &&
            profile_bar_color(60000, p) == p.profile_bar,
        "uniform custom profile color ignored");
  p.profile_by_event = true;
  check(profile_bar_color(0, p) == PaletteRgba{.68f, .34f, .34f, 1} &&
            profile_bar_color(1, p) != profile_bar_color(0, p),
        "default event hue behavior missing");
  const auto legend = status_palette_legend(p);
  check(same(legend[0].color, palette_byte_color(p.trigger)) &&
            same(legend[4].color, palette_byte_color(p.active)),
        "legend not using actual status colors");
}
void invalid() {
  for (auto role : color_palette_roles)
    for (std::size_t component = 0; component < 4; ++component)
      for (float bad : {-.001f, 1.001f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        auto p = default_color_palette();
        palette_color(p, role)[component] = bad;
        check(!valid_color_palette(p), "invalid palette channel accepted");
      }
  bool rejected = false;
  try {
    palette_byte_color({2, 0, 0, 1});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  check(rejected, "invalid RGBA silently clamped");
  rejected = false;
  try {
    auto p = default_color_palette();
    palette_color(p, static_cast<PaletteRole>(99));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  check(rejected, "invalid palette role accepted");
}
} // namespace
int main() {
  try {
    defaults();
    custom();
    invalid();
    std::cout << "palette: legacy defaults/priority/native floats/custom "
                 "colors/validation passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
