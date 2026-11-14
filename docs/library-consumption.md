# Consume the standalone libraries

Each exported target is independent of the viewer. `pvd::core` provides raw
decoding, snapshots, native coordinate access, named clip catalogs and writing; optional `pvd::transport`, `pvd::render`,
`pvd::analysis` and `pvd::repx` provide transport, geometry/native objects,
engineering analysis and XML export. The PhysX SDK is used only by optional
validation executables. ImGui and D3D11 remain in viewer targets.

The external project in `examples/consumer` uses only the installed CMake
package and public headers:

```powershell
cmake --install build-shared --config Release --prefix install-shared
cmake -S examples/consumer -B build-consumer-shared `
  -DCMAKE_PREFIX_PATH=E:/Code/pvd/install-shared `
  -DPVD_CONSUMER_TRANSPORT=ON -DPVD_CONSUMER_RENDER=ON `
  -DPVD_CONSUMER_ANALYSIS=ON -DPVD_CONSUMER_REPX=ON
cmake --build build-consumer-shared --config Release
```

Run the resulting executable with a rigid capture path. On Windows the
installed `bin` directory must be available for DLL lookup. A static install
uses the same consumer project without that DLL requirement. Module options
must match libraries enabled in the installed package; RepX requires render.

The example requires version 0.8 or newer. It verifies core streaming input,
source-sealed clip restoration, native coordinate reads and an in-memory raw save/replay,
optional TCP cancellation, geometry tessellation, native-object extraction,
publication-based scene queries, standalone geometry extraction, captured rigid
velocity overlays, local actor frames, SDK-exact joint line generation, profile/memory/error index
construction, native section timing, native-frame event curves and RepX
generation. It deliberately exports the first completed frame rather than the
file's cleanup tail. Captures with no supported rigid actor require disabling
the example's RepX option. Its explicit partial policy demonstrates diagnostic
export rather than asserting that every source is a complete simulation state.

Core plus analysis can also be built with all viewer/render/transport/RepX
options disabled. The headless configuration runs the same protocol, Session,
writer and analysis tests without ImGui, D3D11 or SDK libraries.
