#pragma once
#include "pvd/render.hpp"

namespace pvd::render {

struct TriangleBuffer {
  std::vector<Vec3> positions, normals;
  std::vector<std::uint32_t> indices;
  std::vector<std::uint16_t> material_indices;
};
struct MeshBuildOptions {
  std::uint32_t radial_segments = 32, hemisphere_rings = 8;
  // Plane is infinite in the extracted model; this is only its finite preview.
  float plane_half_extent = 10;
  std::size_t max_vertices = 4u * 1024u * 1024u;
  std::size_t max_triangles = 8u * 1024u * 1024u;
};

// Produces shape-local triangles, baking mesh/HF scale but not actor/shape
// pose. Analytic rounded shapes have smooth normals; resource meshes use flat
// normals. Reflections reverse winding. Throws for invalid data or output
// budget overflow.
PVD_RENDER_API TriangleBuffer tessellate(const Geometry &,
                                         MeshBuildOptions options = {});

} // namespace pvd::render
