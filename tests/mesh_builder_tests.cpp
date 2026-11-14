#include <cmath>
#include <iostream>
#include <limits>
#include <pvd/mesh_builder.hpp>

using namespace pvd;
using namespace pvd::render;
namespace {
void require(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
}
void near(float a, float b) {
  require(std::isfinite(a) && std::abs(a - b) < 0.0001f,
          "numeric mesh value differs");
}
Vec3 minus(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
void check_mesh(const TriangleBuffer &mesh) {
  require(mesh.positions.size() == mesh.normals.size() &&
              mesh.indices.size() % 3 == 0,
          "invalid triangle buffer shape");
  for (auto n : mesh.normals)
    near(dot(n, n), 1);
  for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
    const auto a = mesh.indices[i], b = mesh.indices[i + 1],
               c = mesh.indices[i + 2];
    require(a < mesh.positions.size() && b < mesh.positions.size() &&
                c < mesh.positions.size(),
            "invalid generated index");
    const auto n = cross(minus(mesh.positions[b], mesh.positions[a]),
                         minus(mesh.positions[c], mesh.positions[a]));
    require(dot(n, mesh.normals[a]) > 0 && dot(n, mesh.normals[b]) > 0 &&
                dot(n, mesh.normals[c]) > 0,
            "winding and normals differ");
  }
}
template <class F> void rejects(F operation) {
  bool caught = false;
  try {
    operation();
  } catch (const std::exception &) {
    caught = true;
  }
  require(caught, "invalid tessellation input was accepted");
}
void primitives() {
  auto box = tessellate(Box{{1, 2, 3}});
  check_mesh(box);
  require(box.positions.size() == 24 && box.indices.size() == 36,
          "box topology differs");
  for (auto v : box.positions) {
    near(std::abs(v.x), 1);
    near(std::abs(v.y), 2);
    near(std::abs(v.z), 3);
  }
  MeshBuildOptions o;
  o.radial_segments = 12;
  o.hemisphere_rings = 3;
  auto sphere = tessellate(Sphere{2}, o);
  check_mesh(sphere);
  require(sphere.positions.size() == 62 && sphere.indices.size() == 360,
          "sphere topology differs");
  for (auto v : sphere.positions)
    near(dot(v, v), 4);
  auto capsule = tessellate(Capsule{2, 3}, o);
  check_mesh(capsule);
  require(capsule.positions.size() == 74 && capsule.indices.size() == 432,
          "capsule topology differs");
  near(capsule.positions.front().x, 5);
  near(capsule.positions.back().x, -5);
  for (auto v : capsule.positions) {
    const auto segment = std::max(-3.0f, std::min(3.0f, v.x));
    near((v.x - segment) * (v.x - segment) + v.y * v.y + v.z * v.z, 4);
  }
  rejects([&] { tessellate(Capsule{2, 0}, o); });
  auto plane = tessellate(Plane{});
  check_mesh(plane);
  for (auto v : plane.positions)
    near(v.x, 0);
  o.radial_segments = 3;
  o.hemisphere_rings = 1;
  check_mesh(tessellate(Sphere{1}, o));
  check_mesh(tessellate(Capsule{1, 2}, o));
}
void scaled_resources() {
  auto source = std::make_shared<MeshData>();
  source->vertices = {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  source->indices = {0, 1, 2};
  source->material_indices = {7};
  auto result = tessellate(TriangleMesh{source, {{-2, 3, 4}, {}}, 0, false});
  check_mesh(result);
  near(result.normals[0].x, -1);
  near(result.positions[1].z, 4);
  near(result.positions[2].y, 3);
  require(result.material_indices == source->material_indices,
          "triangle materials changed");
  require(source->indices == std::vector<std::uint32_t>({0, 1, 2}) &&
              source->vertices[1].y == 1,
          "input resource mutated");
  const float half = std::sqrt(0.5f);
  const auto world =
      transform_point(Transform{{0, 0, half, half}, {3, 4, 5}}, Vec3{1, 0, 0});
  near(world.x, 3);
  near(world.y, 5);
  near(world.z, 5);
  const auto rotated = apply_mesh_scale(
      MeshScale{{2, 1, 3},
                {0, 0, float(std::sin(3.14159265358979323846 / 8)),
                 float(std::cos(3.14159265358979323846 / 8))}},
      {1, 0, 0});
  near(rotated.x, 1.5f);
  near(rotated.y, -0.5f);
  auto hf = std::make_shared<MeshData>();
  hf->vertices = {{0, 0, 0}, {0, 0, 1}, {1, 2, 1}};
  hf->indices = {0, 1, 2};
  auto height = tessellate(HeightField{hf, 2, 2, 0.5f, 2, 3, 0, false});
  check_mesh(height);
  near(height.positions[2].x, 2);
  near(height.positions[2].y, 1);
  near(height.positions[2].z, 3);
  auto convex = tessellate(ConvexMesh{hf, {}, 0});
  check_mesh(convex);
  for (float extent : {1e20f, 1e-30f}) {
    auto extremes = std::make_shared<MeshData>();
    extremes->vertices = {{0, 0, 0}, {extent, 0, 0}, {0, extent, 0}};
    extremes->indices = {0, 1, 2};
    const auto result = tessellate(TriangleMesh{extremes, {}, 0, false});
    near(result.normals[0].x, 0);
    near(result.normals[0].y, 0);
    near(result.normals[0].z, 1);
  }
}
void invalid_inputs() {
  rejects([] { tessellate(Box{{0, 1, 1}}); });
  rejects([] { tessellate(Sphere{-1}); });
  rejects([] { tessellate(Capsule{1, -1}); });
  rejects([] { tessellate(Sphere{std::numeric_limits<float>::infinity()}); });
  MeshBuildOptions o;
  o.radial_segments = 2;
  rejects([&] { tessellate(Sphere{1}, o); });
  o = {};
  o.hemisphere_rings = 0;
  rejects([&] { tessellate(Sphere{1}, o); });
  o = {};
  o.max_vertices = 1;
  rejects([&] { tessellate(Box{{1, 1, 1}}, o); });
  o = {};
  o.radial_segments = std::numeric_limits<std::uint32_t>::max();
  o.hemisphere_rings = std::numeric_limits<std::uint32_t>::max();
  rejects([&] { tessellate(Sphere{1}, o); });
  rejects([] { tessellate(TriangleMesh{}); });
  auto bad = std::make_shared<MeshData>();
  bad->vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  bad->indices = {0, 1};
  rejects([&] { tessellate(TriangleMesh{bad, {}, 0, false}); });
  bad->indices = {0, 1, 7};
  rejects([&] { tessellate(TriangleMesh{bad, {}, 0, false}); });
  bad->indices = {0, 1, 2};
  bad->material_indices = {1, 2};
  rejects([&] { tessellate(TriangleMesh{bad, {}, 0, false}); });
  bad->material_indices.clear();
  rejects([&] { tessellate(TriangleMesh{bad, {{0, 1, 1}, {}}}); });
  rejects([&] { tessellate(TriangleMesh{bad, {{1, 1, 1}, {0, 0, 0, 2}}}); });
  bad->vertices[0].x = std::numeric_limits<float>::quiet_NaN();
  rejects([&] { tessellate(TriangleMesh{bad, {}, 0, false}); });
  bad->vertices = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
  rejects([&] { tessellate(TriangleMesh{bad, {}, 0, false}); });
}
} // namespace
int main() {
  try {
    primitives();
    scaled_resources();
    invalid_inputs();
    std::cout << "mesh builder tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
