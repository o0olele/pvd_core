# UI scale and font atlas lifecycle

`viewer/ui_scale.hpp` is independent of Win32 and GPU backends. It uses the
repository's Dear ImGui 1.91.9b and performs no OS queries, backend initialization,
network access or automatic settings writes. The host selects the current
monitor's scale; `ui_scale_from_dpi(144)` returns `1.5`. Unknown DPI (`0`) falls
back to `1`.

## Host integration

Apply the application's theme at 96-DPI logical sizes, capture that style once,
and construct a UI-thread-owned `UiScale`. The helper is neither copyable nor
movable. Keep it associated with one ImGui context and do not reuse it after that
context is destroyed.

```cpp
apply_theme();
UiScale scale(ImGui::GetStyle(), load_trusted_local_fonts);
// At startup, before any frame/backend font upload:
const auto initial = scale.reload_fonts(monitor_scale);
```

For a monitor change, queue the new scale from the OS message handler. Perform
the rebuild between frames, after the preceding draw data has been submitted or
discarded and before the next `NewFrame`:

```cpp
if (scale.needs_reload(pending_scale)) {
    ImGui_ImplDX11_InvalidateDeviceObjects(); // Clears old atlas TexID.
    try {
        scale.reload_fonts(pending_scale);
    } catch (const std::exception& error) {
        show_error(error.what());
        // The previous CPU atlas/style/default remain usable on failure.
    }
    ImGui_ImplDX11_CreateDeviceObjects(); // Upload current atlas, old or new.
}
ImGui_ImplDX11_NewFrame();
ImGui_ImplWin32_NewFrame();
ImGui::NewFrame();
```

The helper never calls these backend functions itself. The host must also handle
device upload failures. An actual rebuild requires the old `TexID` to be zero.
A repeated request for the already installed scale is a no-op, with
`rebuilt == false`, and need not invalidate or upload anything. `force=true`
requests a fresh atlas even at the same scale.

Rebuilding during an active frame, while a font stack is pushed, on another
thread/context, or using an externally owned/shared atlas is rejected. Sharing an
atlas among multiple contexts requires separate host coordination and is outside
this helper's contract. Old `ImFont*`, atlas pointers and draw data expire after
a successful rebuild. The ImGui context owns and eventually destroys the new
atlas; destroying `UiScale` does not remove fonts.

## Stable style and real font pixels

Every request derives a new style from the saved 96-DPI baseline. It never
rescales the previously rounded style. Thus `1 -> 1.5 -> 2 -> 1` restores even
fractional baseline metrics exactly at `1x`. Other scales use ImGui's own
`ScaleAllSizes`, retaining its intentional integer rounding and unscaled fields.
Colors remain unchanged.

`UiFontLoader` receives a separate empty atlas and an **actual integer pixel
size**, rounded to the nearest pixel from the configured baseline (default 17).
With that default, a `1.5x` request is 25.5 pixels and the packed font is
26 pixels. The viewer host explicitly configures `base_font_pixels = 17` using
`ui::font_size_dp`; its `1x / 1.5x / 2x / 1x` sequence is therefore
`17 / 26 / 34 / 17` pixels. This native baseline was calibrated against actual
screenshots; native glyph height and a browser's CSS font size are not equivalent.
The helper's default remains 17 for other hosts and headless tests. The result
reports both `requested_font_pixels` and `font_pixels`. This avoids ImGui's
implicit truncation hiding the actual raster size. `FontGlobalScale` and the
primary font's `Scale` are `1`; text is rasterized at the requested density.

A loader can append trusted local Segoe UI and merge a local CJK font, returning
the primary `ImFont*`. Its glyph ranges must remain valid until `Build` completes;
ImGui's static glyph-range arrays are suitable. It must use the atlas argument,
honor the supplied size, and avoid changing `GetIO().Fonts` or backend state.
Returning `nullptr` selects ImGui's embedded compressed TrueType font at the same
pixel size. No disk font or network is required for that fallback.

The new atlas is built and validated before publishing any pointer, style or
generation. Invalid loader results, loader exceptions, or resource-limit failures
leave the previous CPU atlas intact. Default limits are scale `0.5..8`, font size
at most 256 pixels, atlas sides at most 16384, and at most 128 MiB of RGBA upload
data. Dimensions and RGBA bytes are checked after packing; the loader and its
local fonts are trusted. These limits do not sandbox a malicious font parser or
bound every temporary allocation inside ImGui's font packing.

## Logical layout sizes

At the beginning of the UI draw function, use a lexical scope:

```cpp
UiPixelScope pixels(scale.current().screen_scale);
ImGui::SetNextItemWidth(ui_pixels(220));
```

`ui_pixels(value)` scales logical layout constants using a thread-local value
whose default is `1`. Nested scopes restore the previous value, including during
exception unwinding. They are neither copyable nor movable and must be destroyed
on the same thread/in normal lexical order. `current_ui_scale()` provides the
current value without borrowing ImGui application data. Existing headless panel
tests without a scope retain their `1x` behavior.

The explicit `ui_pixels(value, scale)` and `UiScale::pixels(value)` variants need
no scope. These functions accept finite nonnegative layout dimensions. Keep
ImGui's negative sentinels such as `-1` and `-FLT_MIN` unchanged. Do not rescale
`GetContentRegionAvail()`, mouse positions, render-target dimensions or other
already physical-pixel values. Style values and font sizes are handled separately
and must not pass through `ui_pixels` again.

The viewer's visual dimensions are declared in `viewer/ui_widgets.hpp`: a
46dp application bar, a 42dp recording bar, a 45dp viewport toolbar, 30dp icon
buttons, 16dp icons and a 17dp native baseline font. The recording bar holds
document tabs on the left and the Scene / Analysis / More controls on the right.
These are logical sizes;
the line icons, rounded timeline thumb and custom tick labels use the same
current scale as the native controls.

Built-in menu, button, tooltip and status labels use English; captured object
names and recording titles retain their original text. Popup and dropdown
surfaces use a shared style so compact application/recording/viewport toolbar
spacing does not leak into menu entries or nested setting selectors.

## Window sizing policy

`plan_ui_window` returns advisory client/outer minimum and initial sizes. Defaults
are a 1060x700 logical client minimum and a 1440x900 logical initial client, with
a 16-logical-pixel work-area margin. Work-area and native non-client chrome
arguments are already physical pixels, so chrome is added once. A small work
area caps even the minimum rather than forcing a window larger than the monitor.
Unknown work areas produce no invented maximum. Very small areas may leave no
client space after native chrome; the host/OS makes the final placement decision.

The host owns per-monitor awareness, `WM_DPICHANGED`, its suggested native RECT,
physical work-area queries, native chrome measurement and `WM_GETMINMAXINFO`.
There is no persisted manual UI-scale setting in this chapter.

The Win32 host enables per-monitor awareness, handles the suggested native RECT,
and applies pending DPI before computing synchronous minimum-size messages. It
queries the monitor work area and adds native chrome once at the OS DPI, even
when `--ui-scale 1.5` overrides the process's UI font/layout scale. Before the
next frame it invalidates DX11 device objects, publishes the newly built atlas,
and uploads the texture. Main layout and panel constants use `UiPixelScope`;
data coordinates, render-target sizes and world-space values stay physical.

## Verification and limits

`tests/ui_scale_tests.cpp` links the real ImGui core. It packs actual embedded
fonts and creates draw data for `1 -> 1.5 -> 2 -> 1`, verifies restored 1x glyph
pixels/text metrics, real increased font sizes, merged CJK glyphs when local
Segoe/YaHei are available, failed-build recovery, GPU-invalidation guards,
active-frame/thread/context/shared-atlas rejection, texture/pixel budgets, nested
thread-local scopes, and constrained work-area sizing. Tests disable ImGui INI
and log files. They do not initialize an OS/GPU backend.

MSVC Debug `/W4` and strict Clang syntax checks passed locally. Physical monitor
switching and mixed-DPI multi-monitor behavior have not been manually verified;
the host's Win32/device integration requires its own validation.

The host has an actual D3D11 WARP `viewer_dpi_gpu` test that sends the same
Win32 DPI message path for 96/144/192/96, checks four font generations and real
texture uploads, and verifies return to original style and client size. The
viewer host's configured font sizes for that sequence are 17/26/34/17 pixels.
Atlas dimensions depend on the installed local fonts and glyph packing; the
test report records the actual dimensions for each generation. Native
maximum/minimum track sizes may constrain large
suggested windows; the report records the actual client rect, not an assumed
unlimited desktop. Additional 1.5x query and 2x scene GPU tests exercise picking
and visible controls. `scaled_panels` uses real ImGui inputs for analysis,
inspection, timeline, native objects, export and workspace controls at all three
scales. These tests do not change physical monitor DPI or replace manual desktop
interaction; the Computer Use helper failed to initialize in this environment.

The same host test also checks first-load auto-fit against the settled viewport.
The first ImGui table layout can briefly expose a one-pixel child width; fitting
that aspect previously placed the camera roughly 600 times too far away in the
real fixture. New sources now wait for two useful matching viewport layouts
before accepting auto-fit. Verified workspace/bookmark cameras remain ready and
are not replaced. The GPU test exercises this initial state before any explicit
fit, selection or frame-navigation action can mask it.
