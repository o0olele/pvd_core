# PhysX 3.4 scene queries

`pvd::render::SceneQueryIndex` extracts the native PVD raycast, sweep and overlap collections, including the separate batched families. The module depends on core data and render value declarations only. It has no SDK, UI, Win32, GPU or network dependency. It does not execute queries again and does not infer missing geometry or hits from the simulated scene.

```cpp
render::SceneQueryIndex index;
const auto queries = index.extract(session, native_frame_marker);
// Or explicitly provide the scene and a complete publication window:
const auto same = index.extract(session,
    render::SceneQueryWindow{scene_id, begin_event_count, end_event_count});
```

The index is incremental. It scans retained **raw events**, including byte-identical property publications that the Session's replay optimization may omit from its state-change index. It resolves property name handles per stream, commits ordinary and `Begin/Append/EndSetPropertyValue` array writes only at their completed publication, records geometry reference mutations, and tracks object generations. `update(session)` can advance it before seeking; backward extraction uses its retained publication history. Reset before binding a different Session, and serialize its queries with Session append. Results own their values and remain valid after another extraction or reset.

## SDK schema and timing evidence

The authoritative local sources are:

- `PhysX_3.4/Source/PhysX/src/NpPvdSceneQueryCollector.h/.cpp`: `PvdReference`, `PvdRaycast`, `PvdSweep`, `PvdOverlap`, `PvdSqHit`, named arrays, clamping and batched result collection.
- `PvdMetaDataPvdBinding.cpp`: `registerPvdRaycast`, `registerPvdSweep`, `registerPvdOverlap`, `registerPvdSqHit`, `sendSceneArray` and `sendSceneQueries` define the exact properties and publication order.
- `PxShared/src/pvd/src/PxPvdDataStream.cpp`: `bufferPropertyValue` writes each native structure and then appends its string fixups.
- `NpSceneQueries.cpp::CapturePvdOnReturn`: collects touches followed by the blocking hit, but explicitly does not use/transmit requested hit flags, cache or filter callback identity.
- `NpBatchQuery.cpp`: actually executes the deprecated 3.4 batch feature and patches successful batch hit ranges.
- `NpScene.cpp::fetchResultsPostContactCallbacks` and `ScbScenePvdClient.cpp::updateSceneQueries`: sends query arrays after scene synchronization and before native frame end, then clears both collectors. Transmission requires `eTRANSMIT_SCENEQUERIES` and PVD debug instrumentation.

Each scene has `SceneQueries.*` and `BatchedQueries.*` properties: `Raycasts`, `Sweeps`, `Overlaps`, `Hits`, `PoseList`, `FilterDataList`, and `GeometryList`. Query records refer to the latter arrays with a serialized array name, base index and count. Geometry list entries are object references to actual temporary `PxBoxGeometry`, `PxSphereGeometry`, `PxCapsuleGeometry` or `PxConvexMeshGeometry` instances. Their fields and referenced mesh resources are read at publication. Previous-frame geometry instances are removed/destroyed and current ones created/published before the query arrays; geometry holders alternate buffers.

These arrays contain native ABI structures, **not** generic scalar streams. After each fixed native record, its pointer-valued `String` properties have appended NUL-terminated C-string fixups in property order. They are not length-prefixed, and are not placed after the entire array. The original native string pointer bytes and arbitrary ABI padding are ignored, never dereferenced. All numeric fields use the source byte order; native pointer fields use `StreamHeader::pointer_width()`, never `sizeof(void*)` of the consumer.

| Record | 32-bit native bytes | 64-bit native bytes | Fixed layout |
| --- | ---: | ---: | --- |
| `PvdReference` | 12 | 16 | pointer, u32 base index, u32 count |
| `PvdRaycast` | 64 | 72 | type, 4 filter words, filter flags, origin, unit direction, distance, hits reference; then hits array-name string |
| `PvdSweep` | 72 | 88 | type, filter flags, unit direction, distance, geometry/pose/filter/hit references; then those four strings |
| `PvdOverlap` | 76 | 88 | type, 4 filter words, filter flags, quaternion/position pose, geometry/hit references; then those two strings |
| `PvdSqHit` | 52 | 64 | shape pointer, actor pointer, u32 face index, u32 flags, position, normal, distance, u, v; source ABI tail padding |

`PoseList` contains 28-byte `physx3::PxTransform` records, with quaternion before position. `FilterDataList` contains 16-byte `physx3::U32Array4` records. The original SDK declarations were independently compiled with both MSVC **Win32 and x64** using `tests/physx34_scene_query_layout.cpp`; every `sizeof` and field `offsetof` in the table is asserted. Thus the 32-bit layout is compiler-verified, not extrapolated from a 64-bit capture.

## Collection, source identity and ranges

Results contain six collection states: native raycast/sweep/overlap, then batched raycast/sweep/overlap. `recorded` includes a freshly sent empty array. `uncollected` means that family had no complete publication in the requested window. An absent publication does not become an empty query result: when collection is disabled, old arrays can remain in the ending snapshot and are deliberately ignored. Missing scenes, malformed arrays, unknown types, stale auxiliary ranges and budgets are reported separately.

Every query retains its scene ID/generation, kind, batched flag, source stream, outer publication timestamp/byte offset, exact publication prefix, and zero-based sequence **within its kind's array**. The collector separates kinds, so neither mixed-kind call order nor a per-query execution timestamp is transmitted. `outer_timestamp` is publication metadata, not query execution time. The byte offset identifies the completing outer event; diagnostic payload offsets are relative to the reassembled opaque array, not absolute offsets through chunked transport events.

The query is decoded from `snapshot_at_event(publication_event_count)`, not a later frame-end state. `SceneQuerySource::snapshot_event_count` therefore equals its publication prefix. A hit actor, shape, geometry or resource destroyed/reused later in the same displayed frame retains its actual publication generation. Unknown references preserve their original unsigned 64-bit IDs and receive an empty resolved key and diagnostic. Known foreign-scene hit actors are reported and not drawn. Reference navigation must check the displayed object's generation before inspecting it.

Array names must match the exact source scene and native/batched family. Ranges are checked with subtraction before addition to prevent unsigned wraparound. Referenced nonempty auxiliary arrays must have a fresh publication/mutation in the same window before the query. This API expects a complete native publication window, not a slice beginning after its hits/geometry were already sent. If a window contains multiple publications of the same family, the result explicitly reports `multiple_publications` and returns only the latest one; it does not silently overlay query history. A custom after-fetch marker can have no query publications at all: callers should explicitly select native frame end rather than relabel an old array as current data.

## What the recording does and does not contain

Raw query type IDs 0–10 are retained. Raycasts distinguish ANY/CLOSEST/ALL in the collector. All sweep IDs use the SDK's `*_SWEEP_CLOSEST_OBJECT` names even when the original call collected multiple hits; the SDK source calls this out as an unused ALL variant. The module preserves that raw type and never treats the name as proof that only one returned hit exists.

`filter_flags` are the recorded `PxQueryFlags`, separate from the four filter data words and **returned** hit flags. Requested `PxHitFlags`, sweep inflation, query cache, filtering callback identity, batch `queryStatus`, `hasBlock` and block/touch boundaries are not serialized. The API exposes omitted input values as empty optionals. In particular, empty recorded hit ranges are called “no recorded hits”, not proof of a successful miss: a failed/overflowed batch result may also leave an empty range. The real SDK oracle knows successful misses independently, but that knowledge is not fabricated in the reader.

Hit position and normal are available only when the returned flags mark them valid; UV additionally requires a raycast and the UV flag, and face availability requires the face flag. Ray/sweep distance is computed by this SDK even though its distance bit is deprecated; negative sweep penetration distance is preserved. Overlap placeholder position/normal/distance/UV fields are never exposed as available impacts. Unavailable raw fields can contain NaNs or other uninitialized-looking values and are retained without being used for geometry. Nonfinite **available** fields fail validation.

`eNO_BLOCK` proves that returned hits are touches and `has_block == false`. Otherwise roles and `has_block` remain unreported: even though the collector appends a block after touches when present, it does not transmit whether one was present. Neither the last hit nor a single hit is silently reclassified as blocking.

Query geometries retain their real geometry instance keys and class names, Box/Sphere/Capsule parameters, or Convex resource ID/key plus mesh scale/rotation/flags. They are not invented actors or PxShapes. The convex scale follows SDK limits `[1e-6,1e6]`; capsule dimensions follow this 3.4 SDK's strictly positive radius and half-height. Convex vertices are not redundantly decoded here: consumers can use the public `Extractor::extract_geometry` on the query's publication snapshot and exact geometry key, reusing its verified resource decoder and cache.

## Derived display data and budgets

Optional `SceneQueryLine` paths use recorded origin or sweep pose position, unit direction and requested maximum distance. The endpoint is a clearly classified **requested path**, not a claimed impact. Impact normals use only an available recorded position and normal, with explicit display length. No point is synthesized from `origin + direction * hit.distance`, no overlap contact point is fabricated, and no omitted hit is reconstructed from scene geometry. Line colors are a viewer palette, not colors collected by the SDK. Every line retains the source query identity, kind and optional hit index and is marked derived. Incomplete queries do not emit lines; nonfinite derived coordinates are diagnosed and omitted.

Default limits are 65,536 queries, 262,144 hits, 262,144 auxiliary items, 64 MiB per raw array, 256 bytes per reference string, 262,144 lines, 256 diagnostics and 1,048,576 index entries. Output hit counts are global across ranges; auxiliary geometry/pose/filter counts are global as well as bounded per source array, preventing many references to one small array from amplifying output without limit. A native-family parse failure commits no partial family. An individual invalid query retains its recorded inputs/ranges and status; fatal auxiliary parsing discards its partial referenced arrays. An unresolved object reference keeps the raw hit record, with unavailable resolved keys. Work and diagnostic limits always leave `complete == false` rather than pretending collection was successful. Invalid options throw before extraction.

## Verification

`tests/scene_queries_tests.cpp` uses the shared test-only wire builder in `tests/scene_query_fixture.hpp`. It covers little/big endian times 32/64 source pointers, unsigned high-bit IDs/filter words/face indices, all three kinds and both families, every truncation of a native query plus string fixup, hostile counts/ranges, type mismatches, unknown kinds, wrong collector names, nonfinite fields, unavailable NaNs, negative sweep penetration distance, sources/actor/geometry generations, publication-time ID reuse and dangling/future references, cross-scene rejection, reverse seeks, incomplete chunks, separate stream handle namespaces, identical no-op publications, absent-frame freshness, multiple-publication windows, independent limits and output amplification budgets.

`examples/physx34_scene_queries_capture.cpp` is a separate SDK-only producer. Expected values come from public query inputs, geometry getters and actual returned hit buffers, not this parser or a second copy of its native-layout decoder. It creates two isolated scenes and three native frames per scene; native and real deprecated batch APIs exercise ray/sweep/overlap hits, successful misses, ANY, NO_BLOCK touches and Box/Sphere/Capsule/Convex inputs. One final scene publishes empty arrays; the other disables transmission, leaving stale prior arrays in state. `tests/physx34_scene_queries.cpp` compares 2,504 independent getter/input fields across 80 queries and 96 hits, validates 128 derived lines, all six native windows, generation keys and reverse seeks. Real batch execution succeeds in this SDK; it is not an omitted or synthetic fallback.

The independent producer, consumer, x64/Win32 ABI projects and logs are under `analysis/scene-query-sdk` and `analysis/scene-query-layout`. Normal library/unit builds require no PhysX installation; the SDK is used only for optional validation.
