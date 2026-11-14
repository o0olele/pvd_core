// Actual Dear ImGui font packing and frame generation, without an OS/GPU
// backend.
#include "imgui_internal.h"
#include "ui_scale.hpp"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace pvd::viewer;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void near(float a, float b, const char *message) {
  check(std::isfinite(a) && std::abs(a - b) < .0001f, message);
}
template <class F> void rejects(F &&f, const char *message) {
  bool caught = false;
  try {
    f();
  } catch (const std::exception &) {
    caught = true;
  }
  check(caught, message);
}
struct Context {
  ImGuiContext *context;
  explicit Context(ImFontAtlas *shared = nullptr)
      : context(ImGui::CreateContext(shared)) {
    ImGui::SetCurrentContext(context);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {1440, 900};
    io.DeltaTime = 1.f / 60;
  }
  ~Context() { ImGui::DestroyContext(context); }
};
ImGuiStyle baseline() {
  ImGuiStyle style;
  style.WindowPadding = {13.25f, 11.75f};
  style.FramePadding = {7.5f, 4.25f};
  style.ItemSpacing = {11.25f, 8.75f};
  style.WindowRounding = 8.5f;
  style.WindowBorderSize = 1.25f;
  style.Colors[ImGuiCol_Button] = {.1f, .3f, .2f, .9f};
  return style;
}
void same_style(const ImGuiStyle &a, const ImGuiStyle &b) {
  near(a.WindowPadding.x, b.WindowPadding.x, "window padding x");
  near(a.WindowPadding.y, b.WindowPadding.y, "window padding y");
  near(a.FramePadding.x, b.FramePadding.x, "frame padding x");
  near(a.FramePadding.y, b.FramePadding.y, "frame padding y");
  near(a.ItemSpacing.x, b.ItemSpacing.x, "item spacing x");
  near(a.ItemSpacing.y, b.ItemSpacing.y, "item spacing y");
  near(a.WindowRounding, b.WindowRounding, "window rounding");
  near(a.WindowBorderSize, b.WindowBorderSize, "unscaled border size");
  near(a.MouseCursorScale, b.MouseCursorScale, "cursor scale");
  near(a.TabCloseButtonMinWidthUnselected, b.TabCloseButtonMinWidthUnselected,
       "tab width sentinel");
  for (int i = 0; i < ImGuiCol_COUNT; ++i) {
    near(a.Colors[i].x, b.Colors[i].x, "style red");
    near(a.Colors[i].y, b.Colors[i].y, "style green");
    near(a.Colors[i].z, b.Colors[i].z, "style blue");
    near(a.Colors[i].w, b.Colors[i].w, "style alpha");
  }
}
std::uint64_t atlas_hash(ImFontAtlas &atlas) {
  unsigned char *data;
  int width, height;
  atlas.GetTexDataAsAlpha8(&data, &width, &height);
  check(data && width > 0 && height > 0, "real atlas pixels exist");
  std::uint64_t hash = 14695981039346656037ull;
  bool nonzero = false;
  for (std::size_t i = 0; i < std::size_t(width) * height; ++i) {
    nonzero |= data[i] != 0;
    hash = (hash ^ data[i]) * 1099511628211ull;
  }
  check(nonzero, "real rasterized glyph pixels exist");
  return hash;
}
float actual_frame(UiScale &scale, bool test_active = false) {
  ImGui::NewFrame();
  if (test_active)
    rejects([&] { scale.reload_fonts(2); }, "active-frame reload rejected");
  ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Always);
  ImGui::SetNextWindowSize({1100, 700}, ImGuiCond_Always);
  ImGui::Begin("DPI frame");
  const float width = ImGui::CalcTextSize("PVD scene properties").x;
  ImGui::TextUnformatted("PVD scene properties");
  ImGui::Button("Timeline / play");
  ImGui::End();
  ImGui::Render();
  check(ImGui::GetDrawData()->TotalVtxCount > 0,
        "actual frame produced glyphs");
  check(ImGui::GetDrawData()->TotalIdxCount > 0,
        "actual frame produced indices");
  return width;
}
void pixel_scope_and_window_policy() {
  near(ui_scale_from_dpi(0), 1, "unknown dpi");
  near(ui_scale_from_dpi(96), 1, "96 dpi");
  near(ui_scale_from_dpi(144), 1.5f, "144 dpi");
  near(ui_scale_from_dpi(192), 2, "192 dpi");
  near(ui_pixels(10), 10, "unscoped pixels");
  {
    UiPixelScope outer(1.5f);
    near(current_ui_scale(), 1.5f, "outer scale");
    near(ui_pixels(10), 15, "outer pixels");
    {
      UiPixelScope inner(2);
      near(ui_pixels(10), 20, "nested pixels");
    }
    near(ui_pixels(10), 15, "nested restoration");
    rejects([] { UiPixelScope invalid(0); }, "invalid scoped scale");
    near(ui_pixels(10), 15, "failed scope keeps previous");
    bool separate = false;
    std::thread other([&] {
      separate = current_ui_scale() == 1 && ui_pixels(10) == 10;
      UiPixelScope worker(3);
      separate &= ui_pixels(10) == 30;
    });
    other.join();
    check(separate, "pixel scale is thread-local");
    try {
      UiPixelScope inner(4);
      throw std::runtime_error("unwind");
    } catch (const std::runtime_error &) {
    }
    near(ui_pixels(10), 15, "exception unwinding restores scope");
  }
  near(current_ui_scale(), 1, "default restored");
  near(ui_pixels(0, 2), 0, "zero metric");
  rejects([] { ui_pixels(-1); }, "ImGui negative sentinel must stay unscaled");
  rejects([] { ui_pixels(std::numeric_limits<float>::max(), 2); },
          "pixel overflow rejected");
  rejects([] { ui_pixels(1, std::numeric_limits<float>::quiet_NaN()); },
          "nonfinite scale rejected");
  const auto normal = plan_ui_window(1.5f, {}, {20, 50});
  near(normal.minimum_client_pixels.x, 1590, "logical minimum scaled once");
  near(normal.minimum_outer_pixels.x, 1610, "physical chrome added once");
  near(normal.initial_outer_pixels.y, 1400, "initial height plus chrome");
  check(!normal.maximum_outer_pixels && !normal.minimum_clamped,
        "unknown monitor does not invent work area");
  const auto small = plan_ui_window(2, ImVec2(1920, 1040), {32, 78});
  check(small.minimum_clamped && small.maximum_outer_pixels,
        "small monitor caps minimum");
  near(small.minimum_outer_pixels.x, 1856, "work margin is logical dpi size");
  near(small.initial_outer_pixels.y, 976,
       "initial window stays inside workarea");
  check(small.initial_outer_pixels.x >= small.minimum_outer_pixels.x &&
            small.initial_outer_pixels.x <= small.maximum_outer_pixels->x,
        "initial bounded by minimum and workarea");
  const auto tiny = plan_ui_window(2, ImVec2(8, 7), {32, 78});
  near(tiny.minimum_outer_pixels.x, 1, "tiny workarea remains bounded");
  near(tiny.minimum_client_pixels.x, 0, "chrome cannot make negative client");
  rejects([] { plan_ui_window(1, ImVec2(0, 1)); }, "zero workarea invalid");
  rejects([] { plan_ui_window(1, {}, {-1, 0}); }, "negative chrome invalid");
  UiWindowPolicy bad;
  bad.work_margin_dp = -1;
  rejects([&] { plan_ui_window(1, {}, {}, bad); }, "negative margin invalid");
}
void real_atlas_roundtrip() {
  const auto base = baseline();
  for (float factor : {1.f, 1.5f, 2.f, 1.f}) {
    auto expected = base;
    if (factor != 1)
      expected.ScaleAllSizes(factor);
    same_style(scaled_ui_style(base, factor), expected);
  }
  auto bad = base;
  bad.WindowPadding.x = std::numeric_limits<float>::infinity();
  rejects([&] { scaled_ui_style(bad, 2); }, "nonfinite style rejected");
  Context context;
  ImGui::GetStyle() = base;
  UiScale scale(base);
  check(scale.needs_reload(1), "initial fonts need explicit build");
  std::uint64_t initial_hash = 0;
  float initial_width = 0;
  std::uint64_t expected_generation = 0;
  for (float factor : {1.f, 1.5f, 2.f, 1.f}) {
    const auto result = scale.reload_fonts(factor);
    check(result.rebuilt && result.used_embedded_fallback,
          "real fallback built");
    check(result.generation == ++expected_generation, "generation increments");
    check(result.atlas == ImGui::GetIO().Fonts &&
              result.primary_font == ImGui::GetIO().FontDefault &&
              result.primary_font == context.context->Font &&
              result.primary_font == context.context->DrawListSharedData.Font,
          "context and shared drawing pointers commit together");
    check(result.atlas->IsBuilt() && result.primary_font->IsLoaded(),
          "real ImGui atlas loaded");
    near(result.requested_font_pixels, 17 * factor,
         "unrounded requested pixels");
    near(result.font_pixels, std::floor(17 * factor + .5f),
         "actual font pixels");
    near(result.primary_font->Scale, 1, "no font stretch");
    near(ImGui::GetIO().FontGlobalScale, 1, "no global stretch");
    check(result.texture_bytes ==
              std::size_t(result.atlas_width) * result.atlas_height * 4,
          "RGBA upload footprint is exact");
    same_style(ImGui::GetStyle(), scaled_ui_style(base, factor));
    const auto hash = atlas_hash(*result.atlas);
    const auto width = actual_frame(scale, factor == 1.5f);
    if (expected_generation == 1) {
      initial_hash = hash;
      initial_width = width;
    } else if (factor == 1) {
      check(hash == initial_hash,
            "1x glyph pixels restored after repeated scale");
      near(width, initial_width, "1x text metrics restored");
    } else {
      check(width > initial_width && hash != initial_hash,
            "larger fonts are rasterized at larger size");
    }
    check(!scale.needs_reload(factor), "same scale needs no rebuild");
    auto *atlas = result.atlas;
    atlas->SetTexID(ImTextureID(99));
    const auto duplicate = scale.reload_fonts(factor);
    check(!duplicate.rebuilt && duplicate.atlas == atlas &&
              duplicate.generation == result.generation,
          "same scale is a genuine no-op even with uploaded atlas");
    rejects([&] { scale.reload_fonts(factor, true); },
            "GPU invalidation required");
    check(ImGui::GetIO().Fonts == atlas, "GPU rejection preserves CPU atlas");
    atlas->SetTexID(ImTextureID{});
  }
  const auto before = scale.current();
  const auto forced = scale.reload_fonts(1, true);
  check(forced.rebuilt && forced.generation == before.generation + 1 &&
            forced.atlas != before.atlas,
        "force rebuild replaces actual atlas");
  bool wrong_thread_rejected = false;
  std::thread worker([&] {
    try {
      scale.reload_fonts(2);
    } catch (const std::logic_error &) {
      wrong_thread_rejected = true;
    }
  });
  worker.join();
  check(wrong_thread_rejected, "wrong thread rejected before ImGui access");
  {
    Context other;
    rejects([&] { scale.reload_fonts(2); }, "different context rejected");
  }
  ImGui::SetCurrentContext(context.context);
  actual_frame(scale);
}
void transactional_failures() {
  Context context;
  const auto base = baseline();
  int mode = 0;
  UiScale scale(base, [&](ImFontAtlas &atlas, float size) -> ImFont * {
    if (mode == 1)
      throw std::runtime_error("font loader failed");
    if (mode == 2)
      return ImGui::GetIO().FontDefault;
    if (mode == 3)
      atlas.TexDesiredWidth = 32768;
    ImFontConfig cfg;
    cfg.SizePixels = mode == 4 ? size + 1 : size;
    return atlas.AddFontDefault(&cfg);
  });
  auto initial = scale.reload_fonts(1);
  const auto hash = atlas_hash(*initial.atlas);
  for (int failure : {1, 2, 3, 4}) {
    mode = failure;
    rejects([&] { scale.reload_fonts(2); }, "invalid staged build rejected");
    check(ImGui::GetIO().Fonts == initial.atlas &&
              ImGui::GetIO().FontDefault == initial.primary_font &&
              scale.current().generation == initial.generation,
          "failed staging keeps published font pointers and generation");
    same_style(ImGui::GetStyle(), base);
    check(atlas_hash(*initial.atlas) == hash,
          "failed staging keeps glyph pixels");
    actual_frame(scale);
  }
  mode = 0;
  check(scale.reload_fonts(2).rebuilt, "failed build is retryable");
  UiScaleOptions tiny;
  tiny.max_texture_bytes = 1;
  UiScale budget(base, {}, tiny);
  auto *existing = ImGui::GetIO().Fonts;
  rejects([&] { budget.reload_fonts(1); }, "RGBA atlas budget enforced");
  check(ImGui::GetIO().Fonts == existing && budget.current().generation == 0,
        "budget failure leaves previous atlas published");
  UiScaleOptions pixel_limit;
  pixel_limit.max_font_pixels = 20;
  UiScale limited(base, {}, pixel_limit);
  rejects([&] { limited.reload_fonts(2); }, "font pixel limit enforced");
  rejects([&] { limited.reload_fonts(.25f); }, "monitor lower bound enforced");
  rejects([&] { limited.reload_fonts(9); }, "monitor upper bound enforced");
  {
    ImFontAtlas shared;
    Context other(&shared);
    UiScale external(base);
    rejects([&] { external.reload_fonts(1); },
            "external/shared atlas rejected");
    check(ImGui::GetIO().Fonts == &shared && shared.Fonts.empty(),
          "shared atlas stays caller-owned and unchanged");
  }
  ImGui::SetCurrentContext(context.context);
}
bool trusted_local_fonts() {
  const auto segoe = std::filesystem::path("C:/Windows/Fonts/segoeui.ttf");
  if (!std::filesystem::exists(segoe))
    return false;
  const bool cjk = std::filesystem::exists("C:/Windows/Fonts/msyh.ttc");
  Context context;
  UiScale scale(baseline(), [&](ImFontAtlas &atlas, float size) -> ImFont * {
    auto *primary = atlas.AddFontFromFileTTF(segoe.string().c_str(), size);
    if (cjk) {
      ImFontConfig cfg;
      cfg.MergeMode = true;
      cfg.PixelSnapH = true;
      atlas.AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", size, &cfg,
                               atlas.GetGlyphRangesChineseSimplifiedCommon());
    }
    return primary;
  });
  for (float factor : {1.f, 1.5f, 2.f, 1.f}) {
    const auto result = scale.reload_fonts(factor);
    check(!result.used_embedded_fallback && result.atlas->Fonts.Size == 1,
          "trusted local font and merged fallback produce one primary");
    if (cjk)
      check(result.primary_font->FindGlyphNoFallback(0x4e2d) != nullptr,
            "CJK glyph actually rasterized after font merge");
    atlas_hash(*result.atlas);
    actual_frame(scale);
  }
  return cjk;
}
} // namespace
int main() {
  try {
    IMGUI_CHECKVERSION();
    pixel_scope_and_window_policy();
    real_atlas_roundtrip();
    transactional_failures();
    const bool cjk = trusted_local_fonts();
    std::cout << "UI scale tests passed: real atlas/frame roundtrip, staging, "
                 "limits, frame/thread/context guards, nested scopes, work area"
              << (cjk ? ", local Segoe+CJK\n" : "\n");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
