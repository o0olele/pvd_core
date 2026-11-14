#include "deformable_panel.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace pvd::viewer {
namespace {
namespace d = deformables;
const char *status(d::DataStatus v) {
  switch (v) {
  case d::DataStatus::available:
    return "Captured";
  case d::DataStatus::not_recorded:
    return "Not recorded";
  case d::DataStatus::disabled:
    return "Read disabled";
  case d::DataStatus::malformed:
    return "Malformed";
  }
  return "Unknown";
}
void availability(const char *label, d::DataStatus value) {
  ImGui::Text("%s: %s", label, status(value));
}
void vector(const char *label, Vec3 value) {
  ImGui::Text("%s: %.6g, %.6g, %.6g", label, double(value.x), double(value.y),
              double(value.z));
}
template <class F> void action(DeformablePanelState &state, F &&fn) {
  try {
    fn();
    state.error.clear();
  } catch (const std::exception &e) {
    state.error = e.what();
  }
}
std::string identity(d::OwnerKey key) {
  return std::to_string(key.scene.id) + ":" +
         std::to_string(key.scene.generation) + "/" +
         std::to_string(key.owner.id) + ":" +
         std::to_string(key.owner.generation);
}
bool match(const std::string &search, const std::string &name, const char *kind,
           d::OwnerKey key) {
  return search.empty() || name.find(search) != std::string::npos ||
         std::string(kind).find(search) != std::string::npos ||
         identity(key).find(search) != std::string::npos;
}
void owner_tools(Document &doc, OrbitCamera &camera, d::OwnerKey key,
                 DeformablePanelState &state) {
  if (ImGui::SmallButton("Fit owner"))
    action(state, [&] {
      if (!doc.fit_deformable(key, camera, state.viewport_aspect))
        throw std::runtime_error(
            "This owner has no captured world positions to fit");
    });
  ImGui::SameLine();
  if (ImGui::SmallButton("Inspect owner"))
    action(state, [&] { doc.inspect(key.owner.id); });
  ImGui::SameLine();
  if (ImGui::SmallButton("Isolate"))
    action(state, [&] { doc.isolate_deformable(key); });
}
void details(Document &doc, const d::ClothSurface &cloth,
             DeformablePanelState &state) {
  ImGui::Text("Native cloth | %zu particles | %zu constraints | %zu fixed",
              cloth.local_positions.size(), cloth.constraints.size(),
              cloth.pinned_indices.size());
  if (cloth.declared_count)
    ImGui::Text("Declared particles: %u", *cloth.declared_count);
  if (cloth.fabric)
    ImGui::Text("Fabric: %llu (generation %llu)",
                static_cast<unsigned long long>(cloth.fabric->id),
                static_cast<unsigned long long>(cloth.fabric->generation));
  if (cloth.sleeping)
    ImGui::Text("Sleeping: %s", *cloth.sleeping ? "yes" : "no");
  availability("Local positions", cloth.local_positions_status);
  availability("World positions", cloth.positions_status);
  availability("Constraints", cloth.constraints_status);
  ImGui::TextWrapped(
      "Surface faces: not recorded by native PhysX 3.4 PVD. Exact particles "
      "and solver constraint edges are shown; edges include bending/shearing "
      "and are not surface topology.");
  ImGui::InputScalar("Element index", ImGuiDataType_U32, &state.element_index);
  if (ImGui::SmallButton("Select vertex"))
    action(state, [&] {
      if (!doc.select_deformable({cloth.key, d::ElementKind::cloth_vertex,
                                  state.element_index,
                                  doc.frame()->snapshot.event_count}))
        throw std::runtime_error(
            "This cloth vertex is not captured in the current sample");
    });
  ImGui::SameLine();
  if (ImGui::SmallButton("Select constraint"))
    action(state, [&] {
      if (!doc.select_deformable({cloth.key, d::ElementKind::cloth_constraint,
                                  state.element_index,
                                  doc.frame()->snapshot.event_count}))
        throw std::runtime_error(
            "This cloth constraint is not captured in the current sample");
    });
  const auto selected = doc.selected_deformable();
  if (!selected || !(selected->object == cloth.key))
    return;
  if (selected->kind == d::ElementKind::cloth_vertex) {
    const auto index = selected->index;
    ImGui::Text("Selected vertex %u (sample %zu)", index,
                selected->sample_event_count);
    if (index < cloth.local_positions.size())
      vector("Local", cloth.local_positions[index]);
    if (index < cloth.world_positions.size())
      vector("World", cloth.world_positions[index]);
    if (index < cloth.inverse_masses.size())
      ImGui::Text("Inverse mass: %.9g%s", double(cloth.inverse_masses[index]),
                  cloth.inverse_masses[index] == 0 ? " (fixed)" : "");
  }
  if (selected->kind == d::ElementKind::cloth_constraint)
    for (const auto &edge : cloth.constraints)
      if (edge.index == selected->index) {
        ImGui::Text("Constraint %u: vertex %u - %u", edge.index, edge.particle0,
                    edge.particle1);
        if (edge.rest_length)
          ImGui::Text("Rest length: %.9g", double(*edge.rest_length));
        if (edge.set_index)
          ImGui::Text("Set: %u", *edge.set_index);
        break;
      }
}
void details(Document &doc, const d::ParticleCloud &cloud,
             DeformablePanelState &state) {
  ImGui::Text("%s | %zu valid particles",
              cloud.kind == d::ParticleKind::fluid ? "Native particle fluid"
                                                   : "Native particle system",
              cloud.particles.size());
  if (cloud.valid_range && cloud.max_particles)
    ImGui::Text("Sparse range: %u | Capacity: %u", *cloud.valid_range,
                *cloud.max_particles);
  if (cloud.base_flags && cloud.read_flags)
    ImGui::Text("Base flags: 0x%X | Read flags: 0x%X", *cloud.base_flags,
                *cloud.read_flags);
  if (cloud.updates_disabled)
    ImGui::TextWrapped(
        "Updates disabled: native PVD skips this owner's updates; displayed "
        "arrays may be the last captured state.");
  availability("Indices", cloud.indices_status);
  availability("Positions", cloud.positions_status);
  availability("Velocities", cloud.velocities_status);
  availability("Flags", cloud.flags_status);
  availability("Rest offsets", cloud.rest_offsets_status);
  availability("Collision normals", cloud.collision_normals_status);
  if (cloud.kind == d::ParticleKind::fluid)
    availability("Densities", cloud.densities_status);
  ImGui::TextWrapped("Indices are original sparse SDK slots. An element "
                     "selection belongs only to the current sample; a reused "
                     "slot is not a persistent particle ID.");
  ImGui::InputScalar("Element index", ImGuiDataType_U32, &state.element_index);
  if (ImGui::SmallButton("Select particle"))
    action(state, [&] {
      if (!doc.select_deformable({cloud.key, d::ElementKind::particle,
                                  state.element_index,
                                  doc.frame()->snapshot.event_count}))
        throw std::runtime_error(
            "That sparse particle index is not valid in this sample");
    });
  const auto selected = doc.selected_deformable();
  if (!selected || !(selected->object == cloud.key) ||
      selected->kind != d::ElementKind::particle)
    return;
  for (const auto &p : cloud.particles)
    if (p.index == selected->index) {
      ImGui::Text("Selected particle %u (sample %zu)", p.index,
                  selected->sample_event_count);
      if (p.position)
        vector("World position", *p.position);
      else
        ImGui::TextDisabled("World position: not recorded");
      if (p.velocity)
        vector("Velocity", *p.velocity);
      if (p.flags)
        ImGui::Text("Flags: 0x%X", *p.flags);
      if (p.density)
        ImGui::Text("Density: %.9g", double(*p.density));
      if (p.rest_offset)
        ImGui::Text("Rest offset: %.9g", double(*p.rest_offset));
      if (p.collision_normal)
        vector("Collision normal", *p.collision_normal);
      break;
    }
}
} // namespace
void drawDeformables(Document &doc, OrbitCamera &camera, RenderOptions &options,
                     DeformablePanelState &state) {
  ImGui::Checkbox("Cloth", &options.show_cloth);
  ImGui::SameLine();
  ImGui::Checkbox("Particles", &options.show_particles);
  ImGui::Checkbox("Constraint edges", &options.show_cloth_constraints);
  ImGui::SameLine();
  ImGui::Checkbox("Fixed-point emphasis", &options.show_cloth_pins);
  ImGui::Checkbox("Particle velocity", &options.show_particle_velocity);
  if (options.show_particle_velocity) {
    ImGui::SetNextItemWidth(ui_pixels(150));
    ImGui::SliderFloat("Velocity scale", &options.particle_velocity_scale, 0, 2,
                       "%.3g");
  }
  if (!doc.loaded()) {
    ImGui::TextWrapped("Open a recording or connect live to inspect native "
                       "PhysX 3.4 cloth and particles.");
    return;
  }
  ImGui::InputTextWithHint("Filter owners",
                           "Name, kind, or owner/scene ID (case-sensitive)",
                           state.search, sizeof(state.search));
  ImGui::Checkbox("Include hidden owners", &state.show_hidden);
  if (ImGui::SmallButton("Restore all visibility"))
    action(state, [&] { doc.show_all(); });
  const auto frame = doc.frame();
  const auto selected = doc.selected_deformable();
  ImGui::Text("%zu cloth | %zu particle owners | sample %zu",
              frame->deformables.cloths.size(),
              frame->deformables.particle_clouds.size(),
              frame->snapshot.event_count);
  if (frame->deformables.cloths.empty() &&
      frame->deformables.particle_clouds.empty())
    ImGui::TextWrapped(
        "No live native cloth/particle owners in this scene/sample. Unrecorded "
        "or external APEX data cannot be reconstructed.");
  const std::string search = state.search;
  const auto row = [&](const auto &owner, const char *kind) {
    if (!match(search, owner.name, kind, owner.key) ||
        (!state.show_hidden && !doc.deformable_visible(owner.key)))
      return;
    const auto id = identity(owner.key);
    ImGui::PushID(id.c_str());
    bool visible = doc.deformable_visible(owner.key);
    if (ImGui::Checkbox("##visible", &visible))
      action(state, [&] { doc.set_deformable_visible(owner.key, visible); });
    ImGui::SameLine();
    const auto label =
        owner.name.empty()
            ? std::string(kind) + " " + std::to_string(owner.key.owner.id)
            : owner.name;
    const bool active = selected && selected->object == owner.key;
    if (ImGui::Selectable(label.c_str(), active))
      action(state, [&] {
        doc.select_deformable({owner.key, d::ElementKind::owner, 0, 0});
      });
    if (active) {
      ImGui::Indent();
      ImGui::TextDisabled(
          "%s | Owner %llu : %llu", kind,
          static_cast<unsigned long long>(owner.key.owner.id),
          static_cast<unsigned long long>(owner.key.owner.generation));
      owner_tools(doc, camera, owner.key, state);
      details(doc, owner, state);
      ImGui::Unindent();
    }
    ImGui::PopID();
  };
  for (const auto &owner : frame->deformables.cloths)
    row(owner, "Cloth");
  for (const auto &owner : frame->deformables.particle_clouds)
    row(owner, owner.kind == d::ParticleKind::fluid ? "Particle fluid"
                                                    : "Particle system");
  if (ImGui::CollapsingHeader("Capture availability and diagnostics")) {
    for (const auto &diagnostic : frame->deformables.diagnostics)
      ImGui::TextWrapped("Owner %llu | %s: %s",
                         static_cast<unsigned long long>(diagnostic.owner.id),
                         diagnostic.property.c_str(),
                         diagnostic.message.c_str());
    if (frame->deformables.suppressed_diagnostics)
      ImGui::TextWrapped(
          "%zu additional diagnostics suppressed by configured limit",
          frame->deformables.suppressed_diagnostics);
  }
  if (!state.error.empty())
    ImGui::TextWrapped("%s", state.error.c_str());
}
} // namespace pvd::viewer
