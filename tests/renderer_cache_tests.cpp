#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "renderer.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
using Microsoft::WRL::ComPtr;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
DrawItem triangle(std::size_t index) {
    auto mesh = std::make_shared<render::TriangleBuffer>();
    mesh->positions = {{-.045f, -.045f, 0}, {.045f, -.045f, 0}, {0, .045f, 0}};
    mesh->normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh->indices = {0, 1, 2};
    DrawItem draw;
    draw.mesh = std::move(mesh);
    draw.key = {{1, 0}, {index + 10, 0}, {index + 1000, 0}};
    draw.world_pose.position = {float(index % 17) * .11f - .88f,
                               float(index / 17) * .11f - .77f, 0};
    draw.color = {220, 80, 40, 255};
    return draw;
}
void working_set(ID3D11Device* device, ID3D11DeviceContext* context) {
    Renderer renderer(device, context); // Production target of 256.
    renderer.resize(320, 240);
    OrbitCamera camera;
    camera.yaw = camera.pitch = 0;
    camera.distance = 4;
    RenderOptions options;
    options.show_grid = options.show_axes = false;
    FrameState frame;
    for (std::size_t i = 0; i < 257; ++i) frame.draws.push_back(triangle(i));
    renderer.render(frame, camera, options);
    const auto cold = renderer.stats();
    check(cold.cache_misses == 257 && cold.cached_meshes == 257 &&
              cold.draw_calls == 257 && cold.triangles == 257,
          "complete visible working set must stay resident beyond LRU target");
    const auto first = readTextureRgba8(device, context, renderer.texture());
    std::size_t visible = 0;
    for (std::size_t i = 0; i < first.pixels.size(); i += 4)
        visible += first.pixels[i] > 40;
    check(visible > 100, "large working set must produce actual GPU pixels");
    renderer.render(frame, camera, options);
    check(renderer.stats().cache_misses == cold.cache_misses &&
              renderer.stats().cache_hits == 257,
          "second render cannot thrash the 257-mesh working set");
    const auto repeated = readTextureRgba8(device, context, renderer.texture());
    check(first.pixels == repeated.pixels,
          "working-set retention must preserve exact rendered pixels");
    camera.orbit(.1, .05);
    camera.pan(5, -3, 240);
    camera.zoom(.1);
    options.show_wireframe = true;
    options.transparent_scope = VisualizationScope::all;
    options.transparent_opacity = .4f;
    renderer.render(frame, camera, options);
    check(renderer.stats().cache_misses == cold.cache_misses &&
              renderer.stats().draw_calls == 514 &&
              renderer.stats().transparent_draws == 257,
          "camera, wire and transparent passes must reuse the visible GPU meshes");
    check(visualization_pick_draws(frame, options).size() == 257,
          "cache residency must preserve CPU picking inputs");
    // Retain all CPU meshes while changing the active subset: weak-owner expiry
    // alone cannot enforce the inactive LRU target in this case.
    for (std::size_t i = 1; i < frame.draws.size(); ++i) frame.draws[i].visible = false;
    options.show_wireframe = false;
    options.transparent_scope = VisualizationScope::none;
    renderer.render(frame, camera, options);
    check(renderer.stats().cached_meshes <= 256 &&
              renderer.stats().cache_misses == cold.cache_misses &&
              renderer.stats().draw_calls == 1,
          "inactive meshes must return to the configured LRU target");
    const auto old_owner = frame.draws[0].mesh;
    frame.draws[0] = triangle(0); // Same attachment, new immutable mesh resource.
    renderer.render(frame, camera, options);
    check(renderer.stats().cache_misses == cold.cache_misses + 1 &&
              renderer.stats().cached_meshes <= 256,
          "replaced mesh ownership must upload new geometry and evict inactive data");
    options.transparent_scope = VisualizationScope::all;
    options.transparent_opacity = 0;
    renderer.render(frame, camera, options);
    check(renderer.stats().draw_calls == 0 &&
              visualization_pick_draws(frame, options).empty(),
          "zero opacity excludes both GPU and CPU picking geometry");
    renderer.clear_mesh_cache();
    check(renderer.stats().cached_meshes == 0, "explicit GPU cache clear");
    for (auto& draw : frame.draws) draw.visible = true;
    options.transparent_scope = VisualizationScope::none;
    renderer.render(frame, camera, options);
    const auto rebuilt = renderer.stats().cache_misses;
    check(rebuilt == cold.cache_misses + 1 + 257,
          "clearing GPU cache must rebuild the complete visible working set");
    renderer.render(frame, camera, options);
    check(renderer.stats().cache_misses == rebuilt,
          "working set remains reusable after explicit cache clear");
}
} // namespace
int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                            nullptr, 0, D3D11_SDK_VERSION, &device,
                                            &level, &context)),
              "create actual WARP device");
        working_set(device.Get(), context.Get());
        std::cout << "GPU working-set retention, camera reuse, inactive eviction, pixels and picking passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
