#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace pvd::viewer {
struct FrameState;
class OrbitCamera;
enum class Antialiasing { off, msaa4x };
struct MultisampleStatus {
    std::uint32_t requested_samples = 1, actual_samples = 1;
    bool fallback = false;
};
// A 4x target requires matching color/depth support and color resolve support.
// The renderer supplies actual device capabilities; exposed for deterministic
// unsupported-device policy testing without pretending WARP lacks 4x support.
MultisampleStatus select_multisampling(Antialiasing, bool color4x,
                                     bool depth4x, bool color_resolve);

struct RenderOptions {
    std::array<float,4> background_color{0.043f,0.063f,0.086f,1};
    Antialiasing antialiasing = Antialiasing::off;
    bool show_grid = true, show_axes = true;
    bool show_solids = true, show_wireframe = false;
    bool show_debug_points = true, show_debug_lines = true, show_debug_triangles = true;
    bool show_contacts = true;
    bool show_joint_frames = true, show_joint_limits = true;
    bool show_scene_queries = true, show_query_normals = true, show_query_volumes = true;
    bool show_bounds = false, show_linear_velocity = false, show_angular_velocity = false;
    float rigid_velocity_scale = 0.2f;
    bool show_cloth = true, show_particles = true;
    bool show_cloth_constraints = true, show_cloth_pins = true;
    bool show_particle_velocity = false;
    float particle_velocity_scale = 0.2f;
    float contact_normal_length = 0.6f;
    float point_size = 5.0f;
};
struct RenderStats {
    std::uint64_t draw_calls = 0, triangles = 0, line_segments = 0, points = 0;
    std::size_t cached_meshes = 0;
    std::uint64_t cache_hits = 0, cache_misses = 0;
    std::uint64_t cloth_points = 0, particle_points = 0,
                  cloth_constraints = 0, particle_velocity_vectors = 0;
    MultisampleStatus multisampling;
    std::uint64_t query_lines = 0, query_volumes = 0;
};
struct RgbaImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> pixels; // Tightly packed top-to-bottom RGBA8.
};

// Accepts a caller-owned immediate context; the renderer retains COM references.
// Rendering is synchronous on the host render thread. No ImGui/window dependency.
// State is set explicitly by each pass; the host's ImGui backend then sets its own.
class Renderer {
public:
    Renderer(ID3D11Device* device, ID3D11DeviceContext* context,
             std::size_t max_cached_meshes = 256);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    // Zero dimensions release viewport targets. Nonzero dimensions are bounded
    // by the device's D3D11 texture limit and validated before allocation.
    void resize(std::uint32_t width, std::uint32_t height);
    void render(const FrameState&, const OrbitCamera&, const RenderOptions& options = {});
    ID3D11ShaderResourceView* shader_resource_view() const noexcept;
    ID3D11Texture2D* texture() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    RenderStats stats() const noexcept;
    void clear_mesh_cache() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Actual GPU readback; accepts RGBA8/BGRA8 UNORM or SRGB textures. Multisampled
// inputs are resolved first. RowPitch is respected and BGRA is converted to RGBA.
RgbaImage readTextureRgba8(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*);
// WIC PNG encoding of that GPU readback. Can also capture the host's complete
// swap-chain backbuffer after ImGui, before Present, including the entire UI.
void saveTexturePng(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*,
                    const std::filesystem::path& path);
} // namespace pvd::viewer
