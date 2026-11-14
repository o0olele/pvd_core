#pragma once
#include "imgui.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>

namespace pvd::viewer {
// Pure value operation: always scale the original 96-DPI style, never a style
// already rounded by a previous monitor scale. At 1x restores the exact base.
ImGuiStyle scaled_ui_style(const ImGuiStyle &base, float screen_scale);
float ui_pixels(float logical_pixels, float screen_scale);
// Scope only logical layout dimensions. It does not change fonts, style or
// framebuffer/input coordinates. A nested scope restores its caller's scale;
// the default on each thread is 1x (including headless panel tests).
class UiPixelScope {
public:
  explicit UiPixelScope(float screen_scale);
  ~UiPixelScope() noexcept;
  UiPixelScope(const UiPixelScope &) = delete;
  UiPixelScope &operator=(const UiPixelScope &) = delete;
  UiPixelScope(UiPixelScope &&) = delete;
  UiPixelScope &operator=(UiPixelScope &&) = delete;

private:
  float previous_;
};
float current_ui_scale() noexcept;
float ui_pixels(float logical_pixels);
float ui_scale_from_dpi(unsigned dpi) noexcept; // Unknown/zero DPI => 1x.
struct UiScaleOptions {
  float base_font_pixels = 17, min_scale = .5f, max_scale = 8;
  int max_font_pixels = 256, max_texture_side = 16384;
  std::size_t max_texture_bytes =
      128u * 1024u * 1024u; // RGBA upload footprint.
};
// Called on a separate, empty atlas. Append trusted local fonts using exactly
// pixel_size; return its primary font, or nullptr for the embedded fallback.
// Glyph-range data must survive Build (static atlas ranges are suitable).
// Do not use ImGui::GetIO().Fonts or initialize a backend in this callback.
using UiFontLoader = std::function<ImFont *(ImFontAtlas &, float pixel_size)>;
struct UiScaleResult {
  float screen_scale = 1, requested_font_pixels = 17, font_pixels = 17;
  ImFontAtlas *atlas = nullptr;
  ImFont *primary_font = nullptr;
  int atlas_width = 0, atlas_height = 0;
  std::size_t texture_bytes = 0;
  std::uint64_t generation = 0;
  bool rebuilt = false, used_embedded_fallback = false;
};
class UiScale {
public:
  // Keep on the UI thread and do not reuse after its ImGui context is
  // destroyed. The context owns the installed atlas; this helper owns no
  // font/GPU objects.
  explicit UiScale(const ImGuiStyle &base_style, UiFontLoader loader = {},
                   UiScaleOptions options = {});
  UiScale(const UiScale &) = delete;
  UiScale &operator=(const UiScale &) = delete;
  UiScale(UiScale &&) = delete;
  UiScale &operator=(UiScale &&) = delete;
  bool needs_reload(float screen_scale) const;
  // Call on the owner thread BETWEEN frames, after submitting/discarding all
  // old draw data. The host must invalidate GPU device/font objects first so
  // old io.Fonts->TexID is zero; then upload/recreate them after this returns.
  // Builds before publishing, installs a context-owned atlas and default font,
  // resets FontGlobalScale=1 and applies scaled base style in one commit.
  // Rejects active frames, external/shared atlases and context changes. It does
  // not initialize/invalidate a GPU/Win32 backend. Old font pointers expire.
  UiScaleResult reload_fonts(float screen_scale, bool force = false);
  const UiScaleResult &current() const noexcept { return current_; }
  float pixels(float logical_pixels) const;

private:
  ImGuiStyle base_style_;
  UiFontLoader loader_;
  UiScaleOptions options_;
  ImGuiContext *context_ = nullptr;
  std::thread::id owner_thread_;
  UiScaleResult current_;
  float font_pixels(float) const;
};
struct UiWindowPolicy {
  ImVec2 minimum_client_dp{1060, 700}, initial_client_dp{1440, 900};
  float work_margin_dp = 16;
};
struct UiWindowPlan {
  ImVec2 minimum_client_pixels, minimum_outer_pixels;
  ImVec2 initial_client_pixels, initial_outer_pixels;
  std::optional<ImVec2> maximum_outer_pixels;
  bool minimum_clamped = false;
};
// Pure advisory policy. Work area and non-client chrome are already PHYSICAL
// pixels; chrome must not be DPI-scaled again. Small work areas cap even the
// minimum instead of forcing a window larger than the monitor. HWND movement,
// WM_DPICHANGED's suggested RECT and native chrome measurement belong to host.
UiWindowPlan plan_ui_window(float screen_scale,
                            std::optional<ImVec2> work_area_pixels = {},
                            ImVec2 non_client_pixels = {},
                            UiWindowPolicy policy = {});
} // namespace pvd::viewer
