#include "ui_scale.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace pvd::viewer {
namespace {
thread_local float pixel_scale = 1;
void scale_valid(float value) {
  if (!std::isfinite(value) || value <= 0)
    throw std::invalid_argument("UI screen scale must be finite and positive");
}
void metric(float value, float scale) {
  const double scaled = double(value) * scale;
  if (!std::isfinite(value) || !std::isfinite(scaled) ||
      scaled <= std::numeric_limits<int>::min() + 1.0 ||
      scaled >= std::numeric_limits<int>::max() - 1.0)
    throw std::invalid_argument(
        "UI style metric is nonfinite or exceeds ImGui integer pixel range");
}
void metrics(ImVec2 value, float scale) {
  metric(value.x, scale);
  metric(value.y, scale);
}
void style_valid(const ImGuiStyle &s, float scale) {
  metrics(s.WindowPadding, scale);
  metric(s.WindowRounding, scale);
  metrics(s.WindowMinSize, scale);
  metric(s.WindowBorderHoverPadding, scale);
  metric(s.ChildRounding, scale);
  metric(s.PopupRounding, scale);
  metrics(s.FramePadding, scale);
  metric(s.FrameRounding, scale);
  metrics(s.ItemSpacing, scale);
  metrics(s.ItemInnerSpacing, scale);
  metrics(s.CellPadding, scale);
  metrics(s.TouchExtraPadding, scale);
  metric(s.IndentSpacing, scale);
  metric(s.ColumnsMinSpacing, scale);
  metric(s.ScrollbarSize, scale);
  metric(s.ScrollbarRounding, scale);
  metric(s.GrabMinSize, scale);
  metric(s.GrabRounding, scale);
  metric(s.LogSliderDeadzone, scale);
  metric(s.ImageBorderSize, scale);
  metric(s.TabRounding, scale);
  for (float width :
       {s.TabCloseButtonMinWidthSelected, s.TabCloseButtonMinWidthUnselected})
    if (width != std::numeric_limits<float>::max())
      metric(width, scale);
  metric(s.TabBarOverlineSize, scale);
  metrics(s.SeparatorTextPadding, scale);
  metrics(s.DisplayWindowPadding, scale);
  metrics(s.DisplaySafeAreaPadding, scale);
  metric(s.MouseCursorScale, scale);
}
struct AtlasDelete {
  void operator()(ImFontAtlas *atlas) const {
    if (atlas)
      IM_DELETE(atlas);
  }
};
using AtlasOwner = std::unique_ptr<ImFontAtlas, AtlasDelete>;
float positive_pixel(double value) {
  if (!std::isfinite(value) || value < 0 ||
      value > std::numeric_limits<float>::max())
    throw std::overflow_error("UI window metric exceeds finite pixel range");
  return static_cast<float>(value);
}
void size_valid(ImVec2 size, bool positive, const char *text) {
  if (!std::isfinite(size.x) || !std::isfinite(size.y) ||
      (positive ? (size.x <= 0 || size.y <= 0) : (size.x < 0 || size.y < 0)))
    throw std::invalid_argument(text);
}
} // namespace
ImGuiStyle scaled_ui_style(const ImGuiStyle &base, float screen_scale) {
  scale_valid(screen_scale);
  style_valid(base, screen_scale);
  ImGuiStyle result = base;
  if (screen_scale != 1)
    result.ScaleAllSizes(screen_scale);
  return result;
}
float ui_pixels(float logical_pixels, float screen_scale) {
  scale_valid(screen_scale);
  if (!std::isfinite(logical_pixels) || logical_pixels < 0)
    throw std::invalid_argument(
        "UI logical pixel metric must be finite and nonnegative; leave ImGui "
        "negative sentinels unscaled");
  return positive_pixel(double(logical_pixels) * screen_scale);
}
UiPixelScope::UiPixelScope(float screen_scale) : previous_(pixel_scale) {
  scale_valid(screen_scale);
  pixel_scale = screen_scale;
}
UiPixelScope::~UiPixelScope() noexcept { pixel_scale = previous_; }
float current_ui_scale() noexcept { return pixel_scale; }
float ui_pixels(float logical_pixels) {
  return ui_pixels(logical_pixels, pixel_scale);
}
float ui_scale_from_dpi(unsigned dpi) noexcept {
  return dpi ? static_cast<float>(dpi) / 96.f : 1.f;
}
UiScale::UiScale(const ImGuiStyle &base_style, UiFontLoader loader,
                 UiScaleOptions options)
    : base_style_(base_style), loader_(std::move(loader)), options_(options),
      owner_thread_(std::this_thread::get_id()) {
  if (!std::isfinite(options.base_font_pixels) ||
      options.base_font_pixels < 1 || !std::isfinite(options.min_scale) ||
      !std::isfinite(options.max_scale) || options.min_scale <= 0 ||
      options.max_scale < options.min_scale || options.max_font_pixels < 1 ||
      options.base_font_pixels > options.max_font_pixels ||
      options.max_texture_side < 1 || !options.max_texture_bytes)
    throw std::invalid_argument("invalid UI scale/font resource options");
  style_valid(base_style, 1);
  current_.requested_font_pixels = options.base_font_pixels;
  current_.font_pixels = std::floor(options.base_font_pixels + .5f);
}
float UiScale::font_pixels(float screen_scale) const {
  scale_valid(screen_scale);
  if (screen_scale < options_.min_scale || screen_scale > options_.max_scale)
    throw std::invalid_argument(
        "monitor scale exceeds configured UI scale range");
  const double requested = double(options_.base_font_pixels) * screen_scale,
               rounded = std::floor(requested + .5);
  if (!std::isfinite(requested) || rounded < 1 ||
      rounded > options_.max_font_pixels)
    throw std::invalid_argument(
        "scaled UI font exceeds configured pixel budget");
  return static_cast<float>(rounded);
}
bool UiScale::needs_reload(float screen_scale) const {
  font_pixels(screen_scale);
  return current_.generation == 0 || screen_scale != current_.screen_scale;
}
float UiScale::pixels(float logical_pixels) const {
  return ui_pixels(logical_pixels, current_.screen_scale);
}
UiScaleResult UiScale::reload_fonts(float screen_scale, bool force) {
  if (std::this_thread::get_id() != owner_thread_)
    throw std::logic_error("UI font rebuild must run on its owner thread");
  const float size = font_pixels(screen_scale);
  const auto style = scaled_ui_style(base_style_, screen_scale);
  auto *context = ImGui::GetCurrentContext();
  if (!context)
    throw std::logic_error("UI font rebuild requires a current ImGui context");
  if (context_ && context_ != context)
    throw std::logic_error("UI scale helper belongs to another ImGui context");
  auto &io = context->IO;
  if (context->WithinFrameScope || !io.Fonts || io.Fonts->Locked ||
      context->FontStack.Size)
    throw std::logic_error("UI fonts/styles can only be rebuilt between frames "
                           "with an empty font stack");
  if (!context->FontAtlasOwnedByContext)
    throw std::logic_error("shared/external font atlases require "
                           "caller-coordinated DPI rebuilding");
  if (current_.generation &&
      (io.Fonts != current_.atlas || io.FontDefault != current_.primary_font))
    throw std::logic_error(
        "font atlas/default was changed outside the UI scale helper");
  if (!force && !needs_reload(screen_scale)) {
    auto result = current_;
    result.rebuilt = false;
    return result;
  }
  if (io.Fonts->TexID != ImTextureID{})
    throw std::logic_error("invalidate the backend font/device objects before "
                           "rebuilding UI fonts");
  if (current_.generation == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("UI font atlas generation exhausted");
  AtlasOwner atlas(IM_NEW(ImFontAtlas)());
  ImFont *primary = loader_ ? loader_(*atlas, size) : nullptr;
  bool fallback = false;
  if (!primary) {
    atlas->Clear();
    ImFontConfig config;
    config.SizePixels = size;
    primary = atlas->AddFontDefault(&config);
    fallback = true;
  }
  bool found = false;
  for (auto *font : atlas->Fonts)
    if (font == primary)
      found = true;
  if (!found)
    throw std::invalid_argument(
        "UI font loader returned a font from another atlas");
  const int desired = atlas->TexDesiredWidth;
  if (desired < 0 || desired > options_.max_texture_side ||
      (desired && (desired & (desired - 1))))
    throw std::invalid_argument(
        "font loader requested an invalid/oversized atlas width");
  if (!atlas->Build() || !atlas->IsBuilt() || !primary->IsLoaded())
    throw std::runtime_error("ImGui font atlas build failed");
  if (primary->ContainerAtlas != atlas.get() || primary->FontSize != size ||
      primary->Scale != 1)
    throw std::invalid_argument("UI font loader must use the requested pixel "
                                "size and unscaled primary font");
  const int width = atlas->TexWidth, height = atlas->TexHeight;
  if (width <= 0 || height <= 0 || width > options_.max_texture_side ||
      height > options_.max_texture_side)
    throw std::length_error("font atlas exceeds configured texture dimensions");
  const auto w = static_cast<std::size_t>(width),
             h = static_cast<std::size_t>(height);
  if (w > options_.max_texture_bytes / 4 / h)
    throw std::length_error(
        "font atlas exceeds configured RGBA texture byte budget");
  UiScaleResult result;
  result.screen_scale = screen_scale;
  result.requested_font_pixels = options_.base_font_pixels * screen_scale;
  result.font_pixels = primary->FontSize;
  result.atlas = atlas.get();
  result.primary_font = primary;
  result.atlas_width = width;
  result.atlas_height = height;
  result.texture_bytes = w * h * 4;
  result.generation = current_.generation + 1;
  result.rebuilt = true;
  result.used_embedded_fallback = fallback;
  auto *old = io.Fonts;
  io.Fonts = atlas.release();
  io.FontDefault = primary;
  io.FontGlobalScale = 1;
  context->Style = style;
  ImGui::SetCurrentFont(primary);
  context_ = context;
  current_ = result;
  IM_DELETE(old);
  return result;
}
UiWindowPlan plan_ui_window(float screen_scale, std::optional<ImVec2> work_area,
                            ImVec2 chrome, UiWindowPolicy policy) {
  scale_valid(screen_scale);
  size_valid(policy.minimum_client_dp, true,
             "UI minimum client size must be finite and positive");
  size_valid(policy.initial_client_dp, true,
             "UI initial client size must be finite and positive");
  size_valid(chrome, false,
             "native chrome extent must be finite and nonnegative");
  if (!std::isfinite(policy.work_margin_dp) || policy.work_margin_dp < 0)
    throw std::invalid_argument(
        "UI work margin must be finite and nonnegative");
  if (work_area)
    size_valid(*work_area, true,
               "monitor work area must be finite and positive");
  UiWindowPlan result;
  auto axis = [&](float minimum, float initial, float nonclient, float work,
                  float &min_client, float &min_outer, float &init_client,
                  float &init_outer, float &maximum) {
    const double desired_min =
                     std::ceil(double(minimum) * screen_scale) + nonclient,
                 desired_initial =
                     std::ceil(double(initial) * screen_scale) + nonclient;
    double cap = std::numeric_limits<double>::infinity();
    if (work_area)
      cap = std::max(
          1., std::floor(double(work) -
                         2 * double(policy.work_margin_dp) * screen_scale));
    const double minimum_outer = std::min(desired_min, cap),
                 initial_outer =
                     std::min(std::max(desired_initial, minimum_outer), cap);
    result.minimum_clamped |= minimum_outer < desired_min;
    min_outer = positive_pixel(minimum_outer);
    min_client = positive_pixel(std::max(0., minimum_outer - nonclient));
    init_outer = positive_pixel(initial_outer);
    init_client = positive_pixel(std::max(0., initial_outer - nonclient));
    if (work_area)
      maximum = positive_pixel(cap);
  };
  ImVec2 maximum;
  axis(policy.minimum_client_dp.x, policy.initial_client_dp.x, chrome.x,
       work_area ? work_area->x : 0, result.minimum_client_pixels.x,
       result.minimum_outer_pixels.x, result.initial_client_pixels.x,
       result.initial_outer_pixels.x, maximum.x);
  axis(policy.minimum_client_dp.y, policy.initial_client_dp.y, chrome.y,
       work_area ? work_area->y : 0, result.minimum_client_pixels.y,
       result.minimum_outer_pixels.y, result.initial_client_pixels.y,
       result.initial_outer_pixels.y, maximum.y);
  if (work_area)
    result.maximum_outer_pixels = maximum;
  return result;
}
} // namespace pvd::viewer
