# Inspector formatting, array groups, and reverse playback

The Inspector's **Inspector settings** popup edits its own `InspectorPreferences`.
The preferences are independent of source data and contain the number format,
float/double precision, and array group size. The public value-only definition
is in `viewer/inspection_preferences.hpp`; it has no ImGui, GPU, or SDK dependency.

General formatting defaults to 9 significant digits for native float values
and 17 for double values, preserving round-trip precision. Scientific and Fixed
use the chosen number of decimal places instead. Precision is selectable from
1 to 17 separately for float and double. A small value can round to zero with
insufficient Fixed decimal places; increase precision or choose General/
Scientific to inspect such values. Scalar/vector/quaternion/transform/bounds/
matrix floating components follow these settings. Integer values, flag words,
indices and object IDs always use exact integer formatting and never pass
through double. The full formatted value is available in the cell tooltip.

Array groups can contain 32, 128, 512 or 2048 source items. Changing group size
returns array pages to their first group. Previous/next controls retain original
indices and bound the last partial group. Visible rows are clipped within the
current group, so a 2048-item group does not require drawing 2048 rows each UI
frame. Array summaries use one compact row per item; multiline source summaries
remain available in tooltips. The source payloads and property values are not
changed, regrouped or re-encoded.

The timeline's **Reverse** checkbox controls the sign of the existing playback
speed. The FPS field edits its absolute magnitude and retains direction. Play,
pause, looping, boundaries, stepping and live-follow semantics continue to use
the existing playback model. Enabling reverse does not jump to an unrelated
frame; start from the desired point (or Last) before playing backward.

`tests/inspection_preferences_tests.cpp` checks little/big endian floating and
64-bit integer displays, tiny values, 17-digit doubles, unchanged source bytes,
all group sizes and last-page boundaries. Real Dear ImGui input switches all
formats and the 2048 group, opens the actual property array, toggles Reverse,
edits speed while retaining direction, and advances the real model backward.
The same interaction tests run at widths 420 and 1060, with visible-row and
horizontal-layout bounds.
