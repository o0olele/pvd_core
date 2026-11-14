# Display color palette

`viewer/palette.hpp` defines an ImGui-, Windows-, and SDK-independent
`ColorPalette`. `ViewOptions::palette` owns the document's display preference;
`FrameState::palette` is the immutable copy used to draw that frame. Palette
changes never modify recorded properties, original event bytes, native coordinate
values, or RepX output.

The Scene tools **Object palette** section edits the static, sleeping, active
dynamic, kinematic, trigger, articulation, cloth, particle-system, and
particle-fluid RGBA colors. All four components must be finite and in `[0,1]`.
The editor applies a validated candidate atomically. **Restore object palette**
restores those nine slots, retaining the separate Profile bar preferences.

The defaults preserve the previous appearance, including its separate system
and fluid colors:

| Role | Default RGBA |
| --- | --- |
| Static | `(220,100,100,255) / 255` |
| Sleeping | `(230,170,70,255) / 255` |
| Active dynamic | `(95,205,125,255) / 255` |
| Kinematic | `(70,135,245,255) / 255` |
| Trigger | `(190,100,220,255) / 255` |
| Articulation in Actor kind mode | `(65,195,200,255) / 255` |
| Cloth | `(0.43,0.69,0.82,1)` |
| Particle system | `(0.91,0.72,0.32,1)` |
| Particle fluid | `(0.26,0.68,0.97,1)` |

In **Recorded/status** mode, a recorded per-object color has first priority.
Otherwise the order is trigger, kinematic, static, sleeping, then active dynamic.
Articulation links keep their sleeping/active status colors in this mode.
**Actor kind** explicitly assigns the articulation color, and other explicit
color modes keep their existing override behavior. Native cloth and particle
colors also take precedence over their palette defaults. Selection highlights,
pins, and diagnostic overlays retain their independent meanings. The status and
actor-kind legends use the same palette helpers as the displayed geometry.

Profile bars retain event-ID hues by default: `profile_by_event=true` uses the
existing HSV mapping `h=((event_id*37)%255)/255`, saturation `0.50`, value `0.68`.
Setting `profile_by_event=false` selects the single `profile_bar` RGBA instead.
The default custom value is `(0.68,0.34,0.34,1)`. The Profile page owns these
controls and its own restore action; changing object colors preserves them.

In the Profiler, expand **Profile bar colors**. **Color bars by event ID**
selects the legacy event hues; editing **Uniform bar RGBA** selects the uniform
custom color. Turning event hues back on keeps the edited custom value.
**Restore Profile colors** resets only `profile_bar` and `profile_by_event`,
leaving the nine object colors untouched. These are per-document display
preferences, with no duplicated state in the analysis index.

`tests/palette_tests.cpp` checks the legacy default priorities, native float
colors, recorded-color preservation, custom colors, and every invalid channel
position. `tests/palette_editor_tests.cpp` uses real Dear ImGui mouse input to
change a color and restore defaults at widths 1060 and 420, checks that Profile
preferences survive both operations, and compares all 65,536 possible event IDs
against the original ImGui HSV implementation.

`tests/profile_palette_tests.cpp` uses real ImGui RGB/alpha mouse edits, mode
switching and Profile restore with LE/BE data at 1x/1.5x/2x and logical widths
420/1060. It inspects the actual timeline-child DrawList vertices, so a colored
editor preview alone cannot satisfy the test. It also checks object-palette
preservation, raw/CSV/source-generation invariance and switching between
documents with different Profile colors.
