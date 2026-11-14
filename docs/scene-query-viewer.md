# Scene query inspection

The Scene queries panel reads the current scene's **native Frame end** window.
It lists the three `SceneQueries` arrays and three `BatchedQueries` arrays
separately: Raycast, Sweep and Overlap. A collection is **recorded**,
**uncollected** or **malformed**. A recorded collection may contain zero
queries. An uncollected window never repeats cached values from an older frame.
After-fetch markers cover narrower validation sections, so this panel offers a
**Use Frame end** button instead of presenting retained arrays as fresh results.

Kind, Native/Batch and text filters affect the list only. Text search covers raw
actor/shape IDs in hexadecimal or decimal, source scene IDs, array names,
query type and filter flags. Query and hit lists use ImGui list clipping; the
core extraction budgets bound their source arrays. The panel caches filtered
indices until the immutable frame or filters change. No decoder, network access
or scene-query execution occurs during drawing.

Selecting a query uses its complete source identity: scene ID and generation,
kind, batch flag, collection-local sequence, snapshot and publication prefixes,
stream ID, outer timestamp and byte offset. The displayed sequence is one based;
it does not imply a global native/batch call order. Seeking, scene replacement
or document replacement cannot retarget that identity to another query.
**Fit query** uses the model's finite visualization bounds, including the
requested path and available hit positions. Query paths, impact normals and
query volumes are independent viewport switches.

Details retain original 64-bit IDs, type/filter flags and all transmitted array
ranges. Auxiliary filter words, poses and geometry are inspectable in bounded
scrolling lists. Hit rows retain both raw IDs even if an instance is absent.
Navigation requires a resolved key whose generation still matches the current
snapshot; a reused pointer is never silently inspected as its previous object.
The hit detail distinguishes each reported field from unavailable data.

PhysX 3.4's PVD scene-query collector does not transmit the original requested
hit flags, sweep inflation, batch result status or `hasBlock`. Ordinary hit
roles therefore remain **unreported**. If `eNO_BLOCK` is present, the extractor
can derive touching-only roles and `hasBlock=false`; the UI labels that inference.
Empty hit ranges are **not a proven miss**. Structural completeness describes
recorded transport data, not query success or exhaustive result collection.

The UI is content only: `drawSceneQueries(Document&, OrbitCamera&,
RenderOptions&, SceneQueryPanelState&)`. The host owns windows, playback ticks
and GPU rendering. State exposes filters, `viewport_aspect`, errors and row
counts; `reset()` releases its value-only frame/list cache.

`tests/scene_query_panel_tests.cpp` drives actual Dear ImGui at widths 420 and
1060 with real mouse events. Its Session-backed synthetic ABI covers 64-bit IDs,
big endian, 301 query records, filters, exact selection, missing arrays,
recorded-empty and uncollected windows, multi-scene separation, object reuse and
native/after-fetch transitions. An optional capture argument exercises the real
SDK producer fixture across all native scene windows.

The real SDK fixture `queries_capture.pxd2` covers two scenes and six native
windows: 80 query records and 96 hit records. Its final two windows distinguish
a recorded-empty collection from disabled/uncollected transport while older
arrays still exist in the snapshot. Both pass the same panel and availability
checks. MSVC Debug `/W4` and strict Clang checks exercise the standalone panel
without a Win32 window, D3D device or PhysX dependency.
