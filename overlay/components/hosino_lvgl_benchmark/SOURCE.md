# LVGL Bench for Brookesia

This is a contained port of the first 15 scenes from LVGL 9.4.0's official
`demos/benchmark/lv_demo_benchmark.c`, not an original substitute workload.
The original MIT license is included in `LICENSE.txt`. Three official image
assets are copied byte-for-byte. `source-manifest.json` records input hashes;
`portengine.patch` records the source transformation.

The original final **Widgets demo** scene is deliberately omitted. It uses
global theme, top-layer UI and additional dynamic resources that need a separate
application port. The imported scene sequence retains the original 3 s / 5 s
durations, primitive creation code, animation parameters and helper functions.
Fifteen scenes take about 49 seconds, plus scheduling and scene construction.

## Integration changes

- All active-screen references target a private scene object inside the app.
  All top-layer references target a private overlay of the same size. No global
  display layer is restyled, cleared or deleted.
- Width/height queries in imported workloads use that private scene's dimensions.
  The app leaves room for the Brookesia status bar, its own header, and global
  FPS HUD/gesture area. Thus workload viewport dimensions differ from a standalone
  full-screen LVGL benchmark; results are not directly comparable with its score.
- Original sysmon observer, scheduling, title and result table are replaced by
  the Brookesia wrapper. `LV_USE_PERF_MONITOR` stays off in the global library.
  `LV_USE_DEMO_BENCHMARK=1` is only a private compile definition for this component
  and the copied assets. The original LVGL benchmark is not globally enabled.
- One controller timer is created in `App::run()` and recorded by Brookesia.
  Scene changes reuse it. Completion and cleanup pause it; Brookesia owns its
  deletion. Scene workloads create animations only, targeting private objects.
- Pause/background entry destroys the current workload and discards its partial
  measurement. Resume recreates and remeasures that same scene. Prior completed
  scene results survive a pause. Closing/reopening or Again starts with clean
  results. No global animation timer is paused or deleted.
- The C engine has one active instance, matching the single registered App.
  All entry points execute with the normal Brookesia/LVGL GUI lock held.

## Measurement

The wrapper samples `hosino_perf_overlay_get_snapshot()` before and after each
scene. Its frame count records the final flush-ready completion of each display
update. FPS is the unsigned count difference divided by actual elapsed microseconds.
Each scene has a separate count; the overall rate is weighted by actual time.
The last in-flight transfer at a scene boundary can contribute to the next window,
so this is a practical device UI benchmark, not a frame-perfect optical trace.
The global HUD and system bar also contribute their small update overhead.

No CPU percentage, rendering time, standardized whole-device score, or optical
refresh rate is claimed. The result list explicitly labels completed UI FPS.
No display event callbacks are installed, so no global display callback cleanup
is required.

## Reproduction

From this component directory, run the installed build Python with:

```powershell
python .\import_lvgl.py --lvgl-root C:\esp\hosino-qspi\project-perf\managed_components\lvgl__lvgl
```

The source component must be the pinned 9.4.0 version. Compare generated hashes
with `source-manifest.json`; the importer only writes inside its own directory.
