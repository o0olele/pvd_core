#include "pvd/mesh_builder.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace pvd::render {
namespace {
constexpr double pi = 3.14159265358979323846;
bool finite(Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
Vec3 triangle_normal(Vec3 a, Vec3 b, Vec3 c) {
  const double ux = double(b.x) - a.x, uy = double(b.y) - a.y,
               uz = double(b.z) - a.z;
  const double vx = double(c.x) - a.x, vy = double(c.y) - a.y,
               vz = double(c.z) - a.z;
  const double x = uy * vz - uz * vy, y = uz * vx - ux * vz,
               z = ux * vy - uy * vx;
  const double length = std::sqrt(x * x + y * y + z * z);
  if (!(length > 0) || !std::isfinite(length))
    throw std::invalid_argument("degenerate or nonfinite mesh triangle");
  return {float(x / length), float(y / length), float(z / length)};
}
void positive(float value) {
  if (!(value > 0) || !std::isfinite(value))
    throw std::invalid_argument(
        "geometry dimension must be finite and positive");
}
std::size_t product(std::size_t a, std::size_t b) {
  if (a && b > std::numeric_limits<std::size_t>::max() / a)
    throw std::length_error("mesh size overflow");
  return a * b;
}
void budget(std::size_t vertices, std::size_t triangles,
            const MeshBuildOptions &o) {
  if (vertices > o.max_vertices || triangles > o.max_triangles ||
      vertices > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("tessellation output exceeds budget");
  (void)product(triangles, 3);
}
void validate_scale(const MeshScale &scale) {
  if (!finite(scale.scale) || !scale.scale.x || !scale.scale.y ||
      !scale.scale.z)
    throw std::invalid_argument("mesh scale must be finite and nonsingular");
  const auto &q = scale.rotation;
  const double norm = double(q.x) * q.x + double(q.y) * q.y +
                      double(q.z) * q.z + double(q.w) * q.w;
  if (!std::isfinite(norm) || std::abs(norm - 1) > 0.0001)
    throw std::invalid_argument(
        "mesh scale rotation must be a unit quaternion");
}
TriangleBuffer box_mesh(Box box, const MeshBuildOptions &o) {
  positive(box.half_extents.x);
  positive(box.half_extents.y);
  positive(box.half_extents.z);
  budget(24, 12, o);
  TriangleBuffer out;
  const float half[] = {box.half_extents.x, box.half_extents.y,
                        box.half_extents.z};
  for (int axis = 0; axis < 3; ++axis)
    for (int sign : {-1, 1}) {
      const int u = (axis + 1) % 3, v = (axis + 2) % 3;
      const auto start = static_cast<std::uint32_t>(out.positions.size());
      for (const auto &uv :
           {std::pair<int, int>{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}) {
        float p[3]{};
        p[axis] = float(sign) * half[axis];
        p[u] = float(uv.first) * half[u];
        p[v] = float(uv.second) * half[v];
        float n[3]{};
        n[axis] = float(sign);
        out.positions.push_back({p[0], p[1], p[2]});
        out.normals.push_back({n[0], n[1], n[2]});
      }
      if (sign > 0)
        out.indices.insert(out.indices.end(), {start, start + 1, start + 2,
                                               start, start + 2, start + 3});
      else
        out.indices.insert(out.indices.end(), {start, start + 2, start + 1,
                                               start, start + 3, start + 2});
    }
  return out;
}
TriangleBuffer plane_mesh(const MeshBuildOptions &o) {
  positive(o.plane_half_extent);
  budget(4, 2, o);
  const auto h = o.plane_half_extent;
  return {{{0, -h, -h}, {0, h, -h}, {0, h, h}, {0, -h, h}},
          {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}},
          {0, 1, 2, 0, 2, 3},
          {}};
}
TriangleBuffer rounded_mesh(float radius, float half,
                            const MeshBuildOptions &o) {
  positive(radius);
  if (!(half >= 0) || !std::isfinite(half))
    throw std::invalid_argument(
        "capsule half height must be finite and nonnegative");
  const auto segments = o.radial_segments, rings = o.hemisphere_rings;
  if (segments < 3 || rings < 1)
    throw std::invalid_argument("rounded tessellation requires >=3 radial "
                                "segments and >=1 hemisphere rings");
  const auto full_rings = product(rings, 2) - (half > 0 ? 0 : 1);
  const auto ring_vertices = product(full_rings, segments);
  if (ring_vertices > std::numeric_limits<std::size_t>::max() - 2)
    throw std::length_error("rounded mesh size overflow");
  budget(ring_vertices + 2, product(product(full_rings, segments), 2), o);
  TriangleBuffer out;
  out.positions.reserve(ring_vertices + 2);
  out.normals.reserve(ring_vertices + 2);
  out.indices.reserve(product(product(product(full_rings, segments), 2), 3));
  std::vector<std::vector<std::uint32_t>> topology;
  auto pole = [&](float x, float nx) {
    const auto id = static_cast<std::uint32_t>(out.positions.size());
    out.positions.push_back({x, 0, 0});
    out.normals.push_back({nx, 0, 0});
    topology.push_back({id});
  };
  auto ring = [&](double alpha, float center) {
    std::vector<std::uint32_t> indices;
    indices.reserve(segments);
    for (std::uint32_t j = 0; j < segments; ++j) {
      const auto theta = 2 * pi * j / segments;
      const Vec3 normal{float(std::cos(alpha)),
                        float(std::sin(alpha) * std::cos(theta)),
                        float(std::sin(alpha) * std::sin(theta))};
      const Vec3 point{center + radius * normal.x, radius * normal.y,
                       radius * normal.z};
      if (!finite(point))
        throw std::invalid_argument("rounded geometry overflow");
      indices.push_back(static_cast<std::uint32_t>(out.positions.size()));
      out.positions.push_back(point);
      out.normals.push_back(normal);
    }
    topology.push_back(std::move(indices));
  };
  if (!std::isfinite(half + radius))
    throw std::invalid_argument("capsule extent overflow");
  pole(half + radius, 1);
  for (std::uint32_t i = 1; i <= rings; ++i)
    ring(pi * 0.5 * i / rings, half);
  if (half > 0)
    ring(pi * 0.5, -half);
  for (std::uint32_t i = 1; i < rings; ++i)
    ring(pi * 0.5 + pi * 0.5 * i / rings, -half);
  pole(-half - radius, -1);
  for (std::size_t i = 1; i < topology.size(); ++i) {
    const auto &a = topology[i - 1];
    const auto &b = topology[i];
    for (std::uint32_t j = 0; j < segments; ++j) {
      const auto next = (j + 1) % segments;
      if (a.size() == 1)
        out.indices.insert(out.indices.end(), {a[0], b[j], b[next]});
      else if (b.size() == 1)
        out.indices.insert(out.indices.end(), {a[j], b[0], a[next]});
      else
        out.indices.insert(out.indices.end(),
                           {a[j], b[j], b[next], a[j], b[next], a[next]});
    }
  }
  return out;
}
TriangleBuffer resource_mesh(const std::shared_ptr<const MeshData> &data,
                             const MeshScale &scale,
                             const MeshBuildOptions &o) {
  if (!data)
    throw std::invalid_argument("mesh resource is missing");
  validate_scale(scale);
  if (data->indices.size() % 3)
    throw std::invalid_argument("mesh index count must be divisible by three");
  const auto triangles = data->indices.size() / 3;
  if (!data->material_indices.empty() &&
      data->material_indices.size() != triangles)
    throw std::invalid_argument("mesh triangle material count differs");
  budget(data->indices.size(), triangles, o);
  TriangleBuffer out;
  out.positions.reserve(data->indices.size());
  out.normals.reserve(data->indices.size());
  out.indices.reserve(data->indices.size());
  for (std::size_t i = 0; i < triangles; ++i) {
    std::uint32_t ids[] = {data->indices[i * 3], data->indices[i * 3 + 1],
                           data->indices[i * 3 + 2]};
    if (scale.flips_winding())
      std::swap(ids[1], ids[2]);
    Vec3 points[3];
    for (std::size_t j = 0; j < 3; ++j) {
      if (ids[j] >= data->vertices.size())
        throw std::invalid_argument("mesh index outside vertex array");
      if (!finite(data->vertices[ids[j]]))
        throw std::invalid_argument("mesh contains nonfinite vertex");
      points[j] = apply_mesh_scale(scale, data->vertices[ids[j]]);
      if (!finite(points[j]))
        throw std::invalid_argument("scaled vertex overflow");
    }
    const auto normal = triangle_normal(points[0], points[1], points[2]);
    for (std::size_t j = 0; j < 3; ++j) {
      out.indices.push_back(static_cast<std::uint32_t>(out.positions.size()));
      out.positions.push_back(points[j]);
      out.normals.push_back(normal);
    }
  }
  out.material_indices = data->material_indices;
  return out;
}
} // namespace
TriangleBuffer tessellate(const Geometry &geometry, MeshBuildOptions options) {
  return std::visit(
      [&](const auto &shape) -> TriangleBuffer {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, Box>)
          return box_mesh(shape, options);
        else if constexpr (std::is_same_v<T, Sphere>)
          return rounded_mesh(shape.radius, 0, options);
        else if constexpr (std::is_same_v<T, Capsule>) {
          positive(shape.half_height);
          return rounded_mesh(shape.radius, shape.half_height, options);
        } else if constexpr (std::is_same_v<T, Plane>)
          return plane_mesh(options);
        else if constexpr (std::is_same_v<T, HeightField>)
          return resource_mesh(shape.mesh,
                               MeshScale{{shape.row_scale, shape.height_scale,
                                          shape.column_scale},
                                         {}},
                               options);
        else
          return resource_mesh(shape.mesh, shape.scale, options);
      },
      geometry);
}
} // namespace pvd::render
