# One-run A/B capture (`host\ab_capture.ps1`)

Proves a graphics effect in one game run instead of 2 to 4: ride a route to a named moment, **freeze game time while the renderer keeps
drawing**, set the effect through a list of values on that one frozen frame, capture each frame and the GPU ms per pass, check the
photo frame, unfreeze and exit. The Off and On frames show the same scene (same animation, camera, particles), so a
difference in the image is the effect.

## Command

```powershell
powershell -File host\ab_capture.ps1 -Route core_florio_a -Moment ride40 -Effect lighting -Set SNAP_LIGHTING=enhanced
powershell -File host\ab_capture.ps1 -Route core_florio_a -Moment ride40 -Effect <registered param or SNAP_NAME> -Values 0,0.5,1 -Set SNAP_LIGHTING=enhanced
```

| switch | meaning |
|---|---|
| `-Route` | a `host\routes\<name>.txt` file; it runs normally until the moment, its A taps stop there |
| `-Moment` | `ride<N>` (rail progress, `ride40` = `ride>=40`), a name from `host\routes\ab_moments.txt`, or a literal route `wait` condition (`ride>=40`, `ui=photo_submit`, `film<=60`) |
| `-Effect` | what to switch, see "Control channel" |
| `-Values` | values set one after the other, default `off,on`. The first value is repeated at the end as a control (noise floor); `-NoControl` skips it |
| `-Set NAME=value ...` | launch-time configuration of the run (for example `SNAP_LIGHTING=enhanced`) |
| `-Settle` / `-Window` | presents to wait after each change (default 12; raise it for temporal effects) / presents timed per value (default 24) |
| `-NoPhoto` | skip the photo-frame check |
| `-Name`, `-OutDir`, `-BuildDir`, `-Timeout`, `-KeepRun`, `-PerfCsv` | output base name (default the moment), output folder (default `analysis\<effect>`), a worker build (`host\build_snap.py --out`), run timeout (590 s), keep the raw dump PNGs, time with `SNAP_PERF_SHARED` rows instead of the dev loop |

The script takes one **normal run slot** from the `host\locks.ps1` fair queue (never an exclusive one), then runs `host\run_boot.ps1 -NoLock` in its own
scratch folder (`build\runs\slotnl<pid>`, deleted afterwards): runs that share slot 0's folder (the slot cap is 1) cannot lock each other's exe copy.
BelowNormal, hidden, muted like every run_boot run.
Exit code: 0 PASS, 10 photo FAIL, 11 photo INCONCLUSIVE, 20 the run produced no result.

## Output

`analysis\<effect>\ab_<moment>.png`, `.json`, and result lines on the console:

* **png**: row 1 the full frames (one per value, the control is left out), row 2 the **most-changed region** magnified. The region is the
  `W/6 x H/6` window with the largest summed pixel difference between value 1 and value 2 (the yellow box on the top row).
* **json**: per value the GPU ms of the frame and of each named pass, the second-minus-first delta of the biggest passes, image metrics (mean abs
  difference, % of pixels that changed by more than 2/255) for first vs second and for the control vs first, the zoom region, the photo-frame result, the
  present numbers of the freeze.
* `FREEZE CHECK: PASS|FAIL`: the control (first value set again at the end) must equal the first frame (under 2% of pixels changed by more than 2/255; the HUD's
  lock-on ring animates by present count and moves about 0.5%). If it fails, time did not hold still and the difference includes motion.
* `PHOTO-FRAME CHECK: PASS|FAIL|INCONCLUSIVE`: see below.

`analysis\` is ignored by git (it holds frames of the game), so the outputs stay local. One real output: `analysis\abcap\ab_ride8.png` / `.json`
(Enhanced lighting, Florio, `-Moment ride8`: Off vs On changes 58% of the pixels, +3.9 ms GPU of which `lfx ao` 3.4 ms; freeze PASS; photo gating PASS).

**Pick a moment where film is left.** The route's own A taps shoot film while it rides: at `ride40` of `core_florio_a` the film is used up and the photo check reads
INCONCLUSIVE ("no film left"). `ride8` (`florio_start` in `ab_moments.txt`) has film.
The beach route (`core_beach_0`) is the same: its shutter taps use all 72 films before `ride40`, so ride it to `beach_start` (`ride>=1`) or `beach_sea` (`ride>=3`).
A run with `-NoPhoto` always ends INCONCLUSIVE (the shutter pass is skipped); the photo check needs a run without it.

**A hash difference needs a baseline.** The check compares the image hash of the first photo frame of each value. Animated water can change it between values with no effect
involved: `core_beach_0`, `beach_sea`, `SNAP_RT_REFLECT` 0 against 1 read FAIL with 4 different photo-frame hashes inside one value and 1.1% of the pixels changed, and the effect
counters showed 0 draws on photo frames. Add a repeated value (`-Values 0,1,0`): if the two 0 frames differ the verdict is INCONCLUSIVE (noise), if they match and the 1 frame differs it is a real FAIL.

## How it works

`host\mods\abcap.cpp` (inert unless `SNAP_ABCAP=<plan file>` is set; the script writes the plan) runs one worker thread, with no guest hook:

1. **Moment**: polls the route `wait` condition through `snap_route_cond` (`runtime\hle\input_core.cpp`, the code a route `wait` line uses). The script appends
   `wait <condition>` to a copy of the route, so the route's own lines run until then and stop for good once the wait is met.
2. **Freeze**: `os_time_pause(1)` (`runtime\os\os_kernel.cpp`). `nn::os::GetSystemTick` stands still, so the game's frame delta is 0: animation, camera, rail and
   particles hold, while the game thread keeps running its frame loop and the renderer keeps submitting and presenting the same frame state.
   Why this hook: it is the one clock the guest reads, needs no guest patch and no change in the render path, is the pause that already exists for window focus
   loss (which blocks the presenting thread, so nothing would render), and on resume the paused span is subtracted, so there is no dt jump. Rejected: the game's pause
   menu (draws a menu over the scene), blocking the frame-step or vsync wait (stops rendering), `SNAP_NEO_SPEED=0` (only the rail), a dt patch in the clock update
   (the clock is copied by value by its readers).
3. **Per value**: set it (control channel), wait `-Settle` presents, record the timing window, wait one second for the dev loop's 1 s averages of the held frame, then request
   one full-size frame (`NVN_FRAME_DUMP_FULL`, the window texture after the upscaler and tonemap, with the game's HUD).
4. **GPU ms per pass**: the dev loop (`SNAP_DEV=1`, `SNAP_DEV_TIMING=1`, `SNAP_DEV_RENDERER_MARKS=1`, `runtime\nvn\nvn_dev.h`): `dev_gpu_busy` and the named `dev_pass_stats`.
   `-PerfCsv` uses `SNAP_PERF_SHARED` / `SNAP_GPU_PASSES` rows instead. The numbers vary by about 1 ms run to run when other runs share the GPU: read the control state too.
5. **Photo frame**, per value:
   * the shutter (A, held 8 presents) with time frozen. If the game takes a photo, the presents `FxIsPhotoFrame()` marks are dumped and hashed: PASS when identical
     for every value, FAIL when they differ while the same value twice is identical.
   * the game's photo flow needs game time: at a frozen moment a fresh shutter does nothing (film counter unchanged, no photo pass; the tail of an earlier shot can still show up as
     photo presents). Then the tool unfreezes, presses A again
     and reports the **gating counters** (`FxRanCount` / `FxRanOnPhotoCount` in `runtime\nvn\nvn_fx.cpp`, counted in `FxNoteRan`): effect passes that drew on presents the Fx plumbing marks as photo
     frames. Since 2026-11-19 a photo taken on a ride holds no present (the effects stay on; only the Evaluation passes of photo select hold), so the tool
     knows a photo was taken from the photo-pass count (`FxPhotoPassCount`, result field `photo_passes`). PASS: a photo pass was seen, 0 draws on held presents and the
     effect drew on other frames; FAIL: any draw on a held frame; INCONCLUSIVE: no photo pass. The scenes differ with time running, so this variant
     does not compare image hashes. An effect that never calls `FxNoteRan` reads INCONCLUSIVE (it must call it when it draws, as the existing ones do).
6. Unfreezes, runs 30 presents more, writes `result.json` and exits.

## Control channel

* `-Effect lighting` (aliases `all`, `ab`): `snaplight::ab_force()` in `runtime\hle\lighting_preset.h`. Every effect that honors `SNAP_LIGHT_AB`
  (`snaplight::ab_on`: Enhanced lighting, SSR, atmosphere, lens effects, the bounce light inside the lighting pass) reads it, so Off is the game's own picture.
  A new effect that gates on `snaplight::ab_on` needs nothing else.
* `-Effect SNAP_NAME` / `NVN_NAME`: with `SNAP_DEV=1` (the script sets it) any live parameter registered with `dev_param_*` (`runtime\nvn\nvn_dev.h`, `docs\devloop.md`) by its env var
  name, with numbers or `off`/`on`. An unregistered name falls back to setting that environment variable live (works for effects that read `getenv` every frame, not for cached statics).

## Notes and limits

* Host effects that animate by present count (noise, flicker, the HUD ring) still move while the game is frozen; the control frame shows how much.
* Launch-time options (shadow-map size and the like) apply to every value; only what the control channel switches live changes between values.
* Needs a worker build that contains `host\mods\abcap.cpp` (any build after the commit that added it).
