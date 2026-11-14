# Inspector formatting, array groups, and reverse playback

The Inspector's **Inspector settings** popup edits its own `InspectorPreferences`.
The preferences are independent of source data and contain the number format,
float/double precision, integer hexadecimal display, and array group size. The public value-only definition
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

**Integer hexadecimal** defaults off. When enabled, unsigned integers use
`0xFEDCBA9876543210`; signed integers use a minus sign and exact magnitude
(`-0x8000000000000000` for `INT64_MIN`). IDs and pointers retain their existing
hexadecimal notation. This preference never converts integers to floating point.

The Inspector consumes metadata `named_values` only with the SDK's declared
`Enumeration Value` or `Bitflag` semantic. Enums require an exact value match;
an unknown enum is not decomposed into bits. Flags show known symbols, unknown
bits in hexadecimal, and the complete raw integer in parentheses. Composite
aliases and zero-valued symbols are retained. For example, a 64-bit flag word
can display `eA | eB | Unknown bits 0x8000000000000000 (9223372036854775811)`.
Properties without these semantics remain numeric rather than guessing names.

Right-click a property value or an expanded array element to copy **native
value**, **last wire value**, or **wire bytes**. Copy selects the original array
index, including on later pages. It copies the complete value rather than the
bounded row/tooltip preview. Native coordinates include recorded OriginShift;
last wire values retain their incoming coordinate epoch, and wire bytes are
the exact selected element's bytes in incoming byte order. Decoded Copy uses
General float 9 / double 17 precision independently of the row's display
precision, preserving tiny/precise values. Integer hex preference and symbolic
metadata are retained. Full strings keep newlines; embedded NUL is shown as
`\u0000` because the clipboard is NUL-terminated. Opaque elements copy all their
bytes. Unavailable typed data and ambiguous byte boundaries fail visibly.

Hosts can set `PanelState::copy_text` to receive the complete text once per
Copy command; an empty callback uses ImGui's clipboard. `Copy ID` uses the same
callback. `propertyValueCopyText` exposes these meanings without a UI. The
callback must not replace the panel or Document during drawing. Workspace v4
persists the hexadecimal preference; versions 1–3 default it off.

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
They also verify LE/BE symbols and unknown flag bits, exact hex extremes,
native/wire Copy, complete long strings/opaque values, malformed typed-value
rejection, and actual mouse clipboard commands on scalar and array rows.
The same interaction tests run at widths 420 and 1060, with visible-row and
horizontal-layout bounds.
