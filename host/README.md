# host/: loader, boot driver and the one-command build

Private personal-use project code; nothing here is game data.

## Build (one command)

    tools/venv-recomp/Scripts/python -I host/build_snap.py [--jobs 8] [--rebuild]

Output: `build/snap/snap_boot.exe` (64 MB, with a linker map). The script:

1. generates `generated/boot/nvn_bridge.cpp` (`host/gen_nvn_bridge.py`: AArch64-to-x86-64 ABI wrappers for every NVN proc, read from the
   C++ definitions in `runtime/nvn`),
2. runs `runtime/hle/gen_hle_stubs.py --all` and copies its stub file to `generated/unimplemented.cpp` (the coverage table is restored; it
   belongs to the HLE owner),
3. compiles (BelowNormal, at most 8 jobs, incremental by mtime) `host/*.cpp`, `runtime/os`, `runtime/hle`, `runtime/audio`, `runtime/nvn`
   and the generated files, using the VS Build Tools environment for the Windows SDK,
4. links with the lifted main and sdk libraries from `build/eh/{main,sdk}/lib` (made by `recomp/snap_build.py`) and `vulkan-1.lib`.

A cold build of the host side takes about 90 s; relinking after a host-only change takes 5 to 10 s. Regenerating and recompiling the lifted
modules is separate (`recomp/snap_gen.py`, `recomp/snap_build.py`).

Build notes (2026-11-14): the incremental check depends on every `host/**/*.h` and `runtime/**/*.h`, and each object has a `.flags` stamp next to it (hash of its compile command), so changing a header in any directory or a code-generation switch (`--pgo-*`, `--lto`, `--cflags-extra`) rebuilds what it affects. `run_boot.ps1` exits with the game's exit code (124 = killed on timeout, route TIMEOUT = 3); `bench.ps1` prints BENCH RUN FAILED for a nonzero code.

## One-command player build (2026-11-16, `host\player_build.ps1`)

`powershell -NoProfile -File host\player_build.ps1` builds the sdk and main libs (`build\eh`), the exe (`build\snap`) and runs the HLE tests; PGO is used when `build\pgo\snap.profdata` exists. Then `powershell -File host\play.ps1`. Options: `-Regenerate` (lifter or ABI edits), `-Retrain` (instrumented build, `florio_photos` training run, merge to `snap.profdata` with the old one kept as `snap.prev-<date>.profdata`), `-NoPgo`, `-SkipTests`, `-DryRun`. Each step takes `build\heavy.lock` alone and releases it; it warns when the profile is older than the generated main code. The profile never leaves this PC (no `.profdata` in git or in a release; `builder\leak_scan.py` rejects it).

## PGO and ThinLTO (2026-11-13, opt worker)

**PGO is the default when `build\pgo\snap.profdata` exists** (local, never committed: it is derived from generated game code).
`recomp\snap_build.py --pgo auto` (default) adds `-fprofile-use` for main and sdk, and a new profile changes the flag stamp, so
the next build recompiles every TU. `host\build_snap.py --pgo-use auto` (default) does the same for the host side; the profile is a
dependency of every host object. Switch back: `snap_build.py --pgo off` and `build_snap.py --pgo-use off --rebuild` (host objects
only notice a new profile, not its removal, so pass `--rebuild` once). A/B result and numbers: `analysis\perf\results.md`, "Build optimization: PGO".

Re-train after main is regenerated (functions whose code changed lose PGO until then; clang skips mismatched ones silently):

    tools\venv-recomp\Scripts\python -I recomp\snap_build.py --gen generated\v262144\sdk --out build\w\<you>\gen\sdk --lib snap_sdk --cflags "-march=x86-64-v3 -fprofile-generate"
    tools\venv-recomp\Scripts\python -I recomp\snap_build.py --gen generated\v262144\main --out build\w\<you>\gen\main --cflags "-march=x86-64-v3 -fprofile-generate"
    tools\venv-recomp\Scripts\python -I host\build_snap.py --out build\w\<you>\gen\exe --pgo-gen --main-lib build\w\<you>\gen\main\lib\snap_main.lib --sdk-lib build\w\<you>\gen\sdk\lib\snap_sdk.lib
    $env:LLVM_PROFILE_FILE = '<abs dir>\snap-%p.profraw'; powershell -File host\run_boot.ps1 -Route florio_photos -BuildDir build\w\<you>\gen\exe -Timeout 590
    tools\llvm\bin\llvm-profdata merge -o build\pgo\snap.profdata <abs dir>\*.profraw      (take build\heavy.lock for this copy)

The instrumented exe runs the whole route at about 50 fps. `host\pgo.cpp` (only in `--pgo-gen` builds) writes the counters every
`SNAP_PGO_DUMP_S` seconds (default 15) because killed runs skip the exit-time write; on Windows each write appends to the file, so it
resets the counters after each write and the merge sums disjoint slices. Check the merge: `llvm-profdata show --function=PresentWindowOnce
--counts` should show about one count per present.

**ThinLTO is opt-in and not recommended**: `snap_build.py --cflags "-march=x86-64-v3 -flto=thin"` (bitcode libs) plus
`build_snap.py --lto thin`. PGO + ThinLTO was no faster than the plain build in two A/B runs, and every relink takes about 140 s
instead of 15 to 60 s. `--cflags-extra` passes extra host compile flags.

## Run

    powershell -File host/run_boot.ps1 [-Timeout 300] [-Log analysis\boot\boot_trace.log] run

runs `snap_boot.exe run` at BelowNormal with a hard timeout (killed by PID). Other commands, all from the project root: `info`, `shared`
(symbols defined in both modules), `findptr <addr>` (slots holding a pointer), `init [--probe N]` (old initializer probe), `eh`, `lua`,
`bench`, `all`. `run` options: `--trace FILE`, `--timeout S`, `--stop-after-init`, `--probe N`, `--stub <mangled>`.
Debug: `SNAP_FNTRACE=0xaddr[,..]` logs every entry of those lifted functions; `SNAP_OS_TRACE=1` logs os-layer thread events;
`NVN_VK_VALIDATION=1` and `NVN_LOG=1` for the NVN layer.

## Play (live input)

    powershell -File host\play.ps1              play
    powershell -File host\play.ps1 -SkipIntro   scripted input through loading, name entry and the lab cutscene (about 3.5 minutes), then it stops and hands control back before the tutorial panels; real input stays active
    powershell -File host\play.ps1 -ListPads    list the controllers the game sees and show live pad input for 15 s

`play.ps1` takes `build\heavy.lock` only while it copies `build\snap\snap_boot.exe` (plus `SDL3.dll`, the PDB and `nvn_cache`) to `build\play\`,
releases it, creates `build\play.lock` (workers must not start boot runs while it exists), runs the copy at normal priority with no timeout and
validation off, and deletes `play.lock` when the game exits (window closed, crash or Ctrl+C). Builds can continue during play. Logs:
`analysis\boot\play_stderr.log`, `play_stdout.log` (device list), `play_trace.log`. Click the game window once so it has focus: keyboard and
gamepad input are ignored while another window is in front.

Saves: the player's save is `%APPDATA%\SnapRecomp\save` (`SNAP_SAVE_DIR` overrides it). `host\run_boot.ps1` and all test runs use
`build\runs\slot<N>\save`, wiped per run (`-KeepSave` keeps it), so tests never touch the player's save. The game writes the profile (language, voice,
appearance, name) when "Are you ready to start?" is confirmed; a later launch skips those screens and goes straight to the opening cutscene. No
later write was seen up to the first tutorial step, so the opening cutscene and tutorial replay until the game saves its progress.

## Window options (`host/window_opts.cpp`, API in `host/window_opts.h`)

F11 or Alt+Enter toggles borderless fullscreen (a `WS_POPUP` window covering the chosen monitor; the NVN swapchain follows through its normal out-of-date path). The windowed
size and position are remembered, and a position on a monitor that is gone is moved to the chosen (or primary) monitor. Settings live in `%APPDATA%\SnapRecomp\window.json`
(`SNAP_CONFIG_DIR` moves the folder), written atomically on every change, on drag end and on close. Test runs (`SNAP_SAVE_DIR` set) do not write it unless `SNAP_WINDOW_SAVE=1`.

| Key | Env override | Default |
|---|---|---|
| `mode` `"windowed"` or `"borderless"` | `SNAP_WINDOW_MODE` | windowed |
| `monitor` (index, -1 = where the window is) | `SNAP_WINDOW_MONITOR` | -1 |
| `width`, `height` (windowed client size), `x`, `y` (outer position), `maximized` | `SNAP_WINDOW_W/H/X/Y` | 1280x720 |
| `pause_on_focus_loss` | `SNAP_WINDOW_PAUSE` | true |
| `mute_in_background` | `SNAP_WINDOW_MUTE_BG` | true |
| `game_notify` (also post nn::oe FocusStateChanged 15 and Resume 16 to the game) | `SNAP_WINDOW_NOTIFY` | false |
| `frame_cap` (fps, 0 = none; only lowers the game's own pacing; needs the nvn `Pace()` line in `analysis\mods
vn_hook_requests.md` #3) | `SNAP_WINDOW_FRAME_CAP` | 0 |

Pause: the game imports no focus-state query (`GetCurrentFocusState`, `SetFocusHandlingMode` are absent; only `TryPopNotificationMessage`, whose one caller at main+0x5dc3f0
turns message 16 into a resume event, 30 into an operation-mode event and anything else into a generic event). So the host pauses: guest ticks freeze (`os_time_pause`), and
the present hook blocks the game thread until focus returns (no frame-delta jump). Mute sets the master volume to 0 and restores it; `window.muted` next to `window.json` restores
it after a crash. With pause on and mute off, the audio thread keeps running on its buffered state.
C API (callable from any thread; applied on the window thread, saved): `snap_window_get_mode/set_mode/toggle_fullscreen`, `snap_window_monitor_count/monitor_name(i)/get_monitor/set_monitor(i)`,
`snap_window_get/set_pause_on_focus_loss`, `snap_window_get/set_mute_in_background`, `snap_window_frame_cap/set_frame_cap`, `snap_window_focused/paused`.
Test helper: `SNAP_WINDOW_TEST="blur@60,focus@75,f11@90,f11@95"` sends WM_ACTIVATE (`blur`, `focus`), WM_KILLFOCUS (`kill`), WM_SETFOCUS (`setfocus`), F11 and Alt+Enter (`altenter`) to the window
at those seconds and logs `[window] test:` lines (paused, master volume, client size) to `boot_stderr.log`. `SNAP_AUDIO_LEVELS=<file>` logs the real output level per second.
`keydown:<key>` and `keyup:<key>` (`escape`, `tab`, `p` ...) hold or release a host key, and `wait:<cond>` (a route wait condition such as `ride>=25` or `ui=main_menu`, no `@time`) waits for the game before the
times of the later events start counting: `SNAP_WINDOW_TEST="wait:ride>=25,blur@3,keydown:escape@4,keydown:tab@5,focus@6,keyup:escape@7,keyup:tab@8"` is a snipping tool whose Esc closes it. Keys held across a focus change
are ignored until released, and Esc and the left mouse button for 300 ms after the focus is back (`SNAP_FOCUS_GUARD=0` turns that off, `SNAP_FOCUS_GUARD_LOG=1` logs Plus and B presses; docs/controls.md, `tests\hle\run_focus_test.ps1`).

Focus test (2026-11-12, `SNAP_WINDOW_TEST="blur@70,focus@90"`, run `analysis\boot\runs\0-20261112_034536`, audio levels in `analysis\boot\audio_levels_win.txt`): blur at 70 s logged `paused=1 master=0.00`; the trace has
no `present` lines between present #3780 (69.9 s) and #3810 (90.7 s), so the guest advanced 0.5 s while 20 s of wall time passed (guest ticks frozen, game thread blocked in the present hook);
the output level was exactly 0 from t=70 to 88 s and about 0.02 rms again after focus; `focus` logged `paused=0 master=1.00` and the cutscene went on. Frame dumps cannot be taken while paused (they run
on the blocked present), so the freeze is shown by the present counter, not by two equal PNGs. The 8 s stall watchdog prints a `STALL` dump during a long pause (harmless, log only). Not tested: F11 and
monitor moves, a real alt-tab, `game_notify`.

Fast boot: `SNAP_FAST_BOOT=1` (optionally `SNAP_FAST_BOOT_FRAMES=<n>`, default 3200) turns the frame limiter off (`nvnHostSetFramePacing(0)`) for the first n presents and back to 60 Hz after. Measured (post_tutorial
save, exclusive runs, trace time of present #3030, when the title logo is up, 3 runs each): without 61.3, 59.1, 56.1 s (average 58.8); with 41.9, 44.2, 41.2 s (average 42.4), about 16 s (28 %) faster.
The first ~2070 presents are black frames (a frame-count-bound load phase with almost no CPU use; holding A from the start did not shorten it): 38.9 s average at 60 Hz, 18.6 s with the limiter off (about 105 fps, bounded by
frame work and FIFO). After that the game is work-bound (about 57 fps even unlimited). More gain needs a present-interval-0 path or cheaper frames in `runtime\nvn` (not touched). No scripted A presses are used: the title waits for the
player (a hands-off run can use `SNAP_INPUT_SCRIPT`).

## Save profiles

Several independent saves live under `%APPDATA%\SnapRecomp\profiles\<id>\save\<uid>\` (same generation layout as before); `profiles.json` holds the
names, last played times and the last used profile.

- First run with the old `%APPDATA%\SnapRecomp\save` folder: it is copied into a profile named "Profile 1", verified, and the old folder is renamed
  `save.migrated-<date>`. Nothing is deleted.
- Startup: with more than one profile a picker window shows name, last played and size, plus "New profile" (mouse, keyboard Up/Down/Enter, controller
  D-pad and A; Esc or B continues with the last played). With one profile there is no picker; hold Shift while starting to see it anyway.
- Direct selection: `host\play.ps1 -Profile "Photo run"` or env `SNAP_PROFILE=<name>` (created when new); env `SNAP_PROFILE_PICKER=0` never shows the picker.
- F1 menu, Saves tab: Create, Rename, Duplicate (copies one consistent generation), Clear save, Delete profile, Open folder, Switch (restarts the
  game), "Import save from folder", "Import test save" (folders in `build\fixtures\saves\`), and "Restore from trash". Clear and Delete move the data to
  `profiles\_trash\<name>-<timestamp>\` (newest 10 kept) after a hold-to-confirm. For the profile that is running, Clear and Delete are queued and applied at the
  next start, before the save mounts. "Restart now" and "Switch profile" relaunch the way `host\play.ps1` starts the game: same exe and arguments, working directory of the first start,
  same environment minus the one-shot variables (`SNAP_INPUT_SCRIPT`, so a `-SkipIntro` script does not replay; the profile picker is not shown again), `SNAP_PROFILE` set
  to the chosen profile. `play.ps1` sets `SNAP_PLAY_ROOT`; the game then writes the new pid to `build\play.restart` and `play.ps1` follows it, keeping `play.lock`.
- `SNAP_SAVE_DIR` still overrides everything (tests); `SNAP_PROFILES_DIR` moves the profiles folder (and keeps profiles on even with `SNAP_SAVE_DIR`).

## Shader precompile screens

Uses the C API in `runtime/nvn/shaderprecompile.h`. First launch (mode not chosen): a prompt offers "Compile shaders now" (with the estimated time; unavailable
until some shaders have been recorded) or "Compile while playing". Mode NOW shows a progress screen (percent, count, time left) with "Continue in background" and
"Cancel", usable by controller. F1 menu, Graphics tab: mode, status, "Compile all known shaders now", and "Rebuild shader cache" (deletes the driver cache,
sets mode NOW and restarts the game). Test runs (`SNAP_SAVE_DIR`, `SNAP_UI_TEST`, `SNAP_SHADER_PRECOMPILE` set) never show the prompt; `SNAP_SHADER_PROMPT=1` forces it.

## Controls menu (F1)

F1, or holding Back/View + Start on a controller for 0.5 s, opens the in-game controls menu (Dear ImGui overlay, native resolution); the same
keys close it (Esc closes too). While it is open the game sees no input. Keyboard and mouse drive it (Q / E, PageUp / PageDown or Ctrl+Tab switch pages, arrows and Tab move through the rows, Enter or Space activates, Esc goes back), and every controller the game accepts drives
it through ImGui gamepad navigation (D-pad or left stick moves, south button activates).

- **Bindings tab:** every Switch input (A B X Y L R ZL ZR + - D-pad, stick clicks, the four directions of each stick) with two slots each for
  Keyboard, Mouse (buttons, X1/X2, wheel up/down) and Controller (buttons, triggers, stick directions). Click a slot, then press the input; Esc cancels,
  Delete or right-click clears. Conflicts are marked with `!` and allowed; two lines under the table name each shared source and the inputs that use it. Per-device "Reset to defaults" buttons and "Reset all controls to defaults". Details, storage and tests: `docs/controls.md`. The launcher's Controls page ends with a read-only summary of the same `input.json`.
- **Mouse tab:** mouse look (right stick, on a ride only; on by default) with sensitivity, invert X/Y and smoothing, touch screen on the left button in menus, cursor lock
  (the cursor is confined to the window while playing; released while the menu is open and when the window loses focus).
- **Controller tab:** face layout (by position, default, or Nintendo labels), swap sticks, inner and outer dead zone and invert Y per stick, ZL/ZR
  thresholds, live stick plots.
- **Controller tab, Gyro aim and Rumble (bottom):** gyro aim is off by default; with a pad that reports a gyroscope (SDL3 sensor API) the angular rate is added
  to the right (camera) stick, with sensitivity (1 = full deflection at 90 deg/s, dead zone 1.5 deg/s) and invert X/Y. Rumble on/off and strength (0 to 1.5) scale what the game sends through
  `SendVibrationValue`; rumble goes through `SDL_RumbleGamepad` (DualSense, Xbox, Switch Pro) with XInput as the fallback. Both apply at once.
- **Settings menu (F1) option pages** (`runtime\ui\ui_options.cpp`, table-driven): Graphics (render scale, anti-aliasing, sharpening, anisotropic filtering, texture sharpness, shadow resolution, ambient occlusion, color look and strength, HDR + peak and paper-white nits, frame cap, UI font scale, field of view 1.0-1.4 with `SNAP_FOV_SITES=0x296400,0x38cdbc`, detail distance (Original, High, Max), always full detail, draw distance 1-8 and full-rate distant animation, the last four marked "may change photo scores, being tested"), Display (window mode, monitor, windowed size, pause when unfocused, mute in background, fast boot, button icons: automatic, Switch, Xbox or PlayStation pictures for the game's prompts, live, `SNAP_BUTTON_ICONS`; see `analysis\mods\button_icons.md`), Photos (PNG export on/off, folder, open folder, gallery), Mods (packs with enable box and load order, written to `mods.json`) and the output mode on the Audio tab. Every row shows "applies now" or "restart needed" (yellow "changed: restart to apply" once changed); a help panel on the right gives plain-language help, the default, the effective render size and the env override name. A bar under the title counts pending restart changes and "Apply and restart" calls `restart_now`.
- **Upscaler rows (live, restart):** Upscaler (`SNAP_UPSCALER`: off, DLAA, DLSS, FSR, XeSS) and Upscaler quality (`SNAP_UPSCALE_QUALITY`: native, quality, balanced, performance; the help text lists the render size of each for a 4K window). They are not part of the quality presets. **Coming soon (disabled row, env wired in `settings.cpp`):** Lighting preset (`SNAP_LIGHTING`). Flip `kSoon` in the option table when the feature code lands.
- **Where values live:** graphics, photo and fast-boot keys in the per-profile `settings.cfg` (exported as env vars at start, a hand-set env wins; "Default" erases the key); window mode, monitor and frame cap in `window.json`; audio output in `audio.json`; mods in `mods.json`.
- **Photos tab:** PNG export options plus the gallery: starts `tools-src\gallery\serve.py --no-open` for the active profile with `--port-file`; the server tries 8777 then any free port (8765 is taken by other software on some PCs) and the tab opens the port it reports.
- **Options test:** `tests\hle\run_options_test.ps1 -BuildDir <dir>` runs stage A (sets options through `SNAP_UI_TEST`, toggles and reorders mods, Apply and restart) then stage B (a fresh run on the same profile: checks saved values, exported env vars and mods order) and writes `analysis\ui\options_test.log`; `tests\hle\opt_shots.py` makes `analysis\ui\f1_*.png`. Verified: stage A 23 PASS, stage B 22 checks 0 failed; the game log shows `aa: mode smaa, sharpen 0.30` and `gamemods: UI font scale 2` after the restart. Settings load only when `SNAP_PROFILES_DIR` is set (tests also set `SNAP_PROFILE` and `SNAP_PROFILE_PICKER=0`); in hidden runs the relaunched child dies with the run, hence two runs.
- **Per-profile settings** live in `%APPDATA%\SnapRecomp\profiles\<id>\settings.cfg` (flat `key=value`; `runtime\hle\settings.cpp`): `look`, `look_intensity`, `ssao_quality`,
  `gyro_on`, `gyro_sens`, `gyro_invert_x/y`, `rumble_on`, `rumble_strength`, `button_icons`. Loaded in `hle::activate_profiles()`; test runs with `SNAP_SAVE_DIR` do not load them.
- **Accessibility tab:** color-vision correction, reduce flashing, text and menu size, high-contrast menus, mono audio, and hold-to-toggle for ZL, ZR and the A/B/X/Y/L/R buttons (`toggle_buttons` in `input.json`); the launcher has a matching Accessibility page. Every row has a `SNAP_ACCESS_*` variable and a settings key; see `docs/accessibility.md`.  **Test input tab:** live readout of what npad 0 would report.
- **Cheats tab:** an "Allow cheats" switch and one checkbox per cheat from your own Atmosphere-format file, `cheats/<build id>.txt` next to the exe (the project ships none). Off by default: nothing is read and nothing runs until the switch is on. See `docs/cheats.md`.
- Settings apply live and are saved atomically to `%APPDATA%\SnapRecomp\input.json` (`SNAP_CONFIG_DIR` overrides the folder). An invalid file is kept as
  `input.json.invalid`, defaults are used and a notice shows on screen. The old `config\input_map.json` is migrated once. Notices show
  once per event for 8 s (fading over the last 1.5 s) and name files only, never a full path (the log line keeps the paths).
- Test: `tests\hle
un_tests.ps1` (binding resolution, JSON round trip, migration, capture) and `tests\hle
un_ui_test.ps1` (boots the game, posts F1,
  rebinds a key, checks the npad; `SNAP_UI_TEST=<script>`).

Default controls (before any edit). Buttons use the Switch layout by position, so on an Xbox pad Switch A is the right face button (Xbox B) and
Switch B is the bottom one (Xbox A); the Controller tab can switch to Nintendo labels.

| Switch | Gamepad (XInput or SDL3: Xbox, PlayStation, Switch Pro) | Keyboard |
|---|---|---|
| A / B / X / Y | east / south / north / west face button | L or Enter / K or 1 / I or 3 / J or 2 |
| Left stick (move) | left stick | W A S D |
| Right stick (aim camera) | right stick | arrow keys |
| L / R | left / right shoulder | Q / E |
| ZL / ZR | left / right trigger | Left Shift or mouse wheel / Space |
| Plus / Minus | Start / Back | P or Tab / Backspace (Esc is Back in menus, Plus on a ride) |
| D-pad | D-pad | T / F / H / G (up, left, right, down) |
| Touch screen | | left mouse button in every menu and on every non-ride screen (and on a ride with mouse look off) |
| A (Take Photo) / ZL (Zoom) during a ride | | left / right mouse button (mouse look is on by default; the mouse aims only on a ride) |

Input switches (environment, override the profile): `SNAP_RUMBLE=0..1.5` (strength, 0 = off), `SNAP_RUMBLE_LOG=1` (log each game vibration request and
host rumble call), `SNAP_GYRO=0|1` (gyro aim on the camera stick; needs a pad with a gyro; untested on real hardware), `SNAP_GYRO_SENS=<n>` (1 = full
stick at 90 deg/s), `SNAP_GYRO_FAKE=<yaw_dps>,<pitch_dps>` (pretend gyro for tests; yaw positive turns left, pitch positive tips up; implies `SNAP_GYRO=1`).
Test routes: `host\routes\rumble_probe.txt`, `host\routes\gyro_probe.txt`; input-script tokens `rumble=<lo>,<hi>` and `gyro=<yaw>,<pitch>` (pretend gyro, needs `SNAP_GYRO=1`). The token `src=<key:P|mouse:left|pad:east>` holds a host source through the binding table, so a rebound key can be tested end to end (`host/routes/rebind_shutter.txt`, `docs/controls.md`).
Mouse aim (Controls, Mouse tab) has mouse-look, sensitivity, invert and smoothing (`mouse.sensitivity`, `mouse.smoothing` in `input.json`). Mouse look is on by default (2026-11-16): during a ride the mouse moves the camera, the left button is A and the right button is ZL (held while held); in menus and every other screen the cursor is free and a click is the touch screen. The host tells a ride from a menu by the camera rail advancing. Esc is B in menus and Plus (pause) on a ride; the wheel zooms (ZL). The prompts in the F1 menu and in game screens show key caps when the last input was the keyboard or mouse (Controller tab, Button icons: Auto follows the last input, or force Keyboard and mouse). The two click bindings are `mouse.look_click_a` and `mouse.look_click_zl` in `input.json` (arrays of sources such as `mouse:left`, `mouse:middle`, `key:L`; `[]` turns one off). They are not on the Bindings tab. Files saved with mouse look off keep it off. The token `mouse=<x>,<y>` (0..1 of the window) puts the cursor there for route tests (menu clicks with `src=mouse:left`).

The game polls only the Full-key (Pro controller) npad 0 state, the touch screen and gestures; it never asks for six-axis (motion) data.
Pads: every gamepad SDL3 reports is opened and merged with every XInput slot into npad 0 (buttons OR-ed, larger stick value wins), the
joystick list is rescanned every second (hot-plug, wake-up) and each device is printed (`pad:` lines in `play_stdout.log`). SDL3 is used when
`tools\dl_sdl3\x\SDL3-3.4.20` exists (the build copies `SDL3.dll` next to the exe); XInput is always read as well.
The player name prompt is answered automatically (no on-screen keyboard): the name is the setting `player_name` in `hle.json` (`%APPDATA%\SnapRecomp\config\hle.json`), default "Ash"; the env var `SNAP_PLAYER_NAME` overrides it. UTF-8 is accepted; the game's own length limit applies.
Scripted input for unattended runs: `SNAP_INPUT_SCRIPT=<file>` (lines `<start> <hold> <buttons or lx= ly= rx= ry=>`, `repeat <start> <end> <period>
<hold> ...` and `anchor <event>`; seconds since the first poll or since the event), OR-ed onto the real input. `host\skip_intro.txt` is the intro script.

### Frame-based script times (load-independent)

A script line whose start is written `@f<frame>` counts guest presents (the NVN present count) since the segment began instead of seconds:
`@f600 30 A` holds A from present 600 for 30 presents; `repeat @f0 @f3000 90 15 A` taps A for 15 presents every 90. End, period and hold take
an optional `@f` prefix (frames either way on such a line). Time lines and frame lines can be mixed in one file. At 60 fps 60 presents is
1 s; under load the game just takes longer in wall time and the route stays in step. Implemented in `runtime\hle\input_core.cpp`
(the script player lives there, not in `hid.cpp`).

Photo-score comparisons between frame rates must use game-second taps (`host/routes/score_time_a.txt`): present-indexed `@f` taps fire 2.4x as often at 144 as at 60 and shoot different photos.

### Waiting on game state (`wait`, `end`; 2026-11-13)

Present counts still drift by thousands between runs (load times, ride phase), so routes wait on the game instead:

    wait <condition> [timeout=<s>] [for=<n>]
    end [<start>]

`wait` starts a new segment (like `anchor`) once the condition holds for `n` presents in a row (default 1); the lines after it count
from that moment (`@f0` = the present it was met). Until then the lines of the segment before keep running. `timeout` (default 300 s,
`0` = none) counts wall seconds from the start of the segment before; when it runs out the log gets `input script: TIMEOUT after <s> s
waiting for condition '<c>'` plus one line per known UI scene, rail, film and dialog state, and the run exits with code 3 (set
`SNAP_ROUTE_TIMEOUT=continue` to log and keep going). `anchor` lines take `timeout=` too. `end` exits the run with code 0 at that time
of its segment (a finished route need not sit out `-Timeout`). Every met wait logs `input script: wait '<c>' met after <s> s at present
<n>`. A `!` in front negates a condition.

| condition | true when | read from |
|---|---|---|
| `ui=<scene>` | that Lumen UI scene is open (between its "opened" and "closed" events). Names are the `.drp` base names: `title`, `main_menu` (lab hub menu), `map_select`, `photo_submit` (photo select), `evaluation`, `pause_menu`, `option`, `result_reward`, `myphoto`, ... | inline hook `rt_gm_route_ui_event` at `main+0x6c1d90` (`host\mods\routes.cpp`): the scene base's event handler; scene name from the Lumen player (`+0x10`) |
| `ui=<scene>:<state>` | open and in that layout state (`State*` = prefix). Seen: `title:PressStart`, `main_menu:MainMenu`, `map_select:Start` / `StageSelect` / `CourseSelect` / `Exit`, `photo_submit:FolderSelect` (subject folders) / `PhotoSubmit` (one folder's photos; A on a photo picks it and goes back to the folders), `evaluation:Start`, then per photo `EvaluationNew` / `Comment` / `Score` / `Announce` / `PhotoRegister`, then `End` | same hook (state string of each event) and `rt_gm_route_ui_setstate` at `main+0x79a6d0` (Player::SetState) |
| `dialog` | the dialogue / skit text window waits for A (seen for the professor before photo select; not every window uses it) | existing hook `rt_gm_skip_wait` at `main+0x676630` (`host\mods\skips.cpp`) |
| `ride` | the NEO-ONE rail moves (progress advanced in the last 20 presents) | existing hook `rt_gm_neo_speed` at `main+0x4c1df4` (`neofilm.cpp`): rail object `+0x70` |
| `ride>=<x>` | moving and the rail progress is at least x (Florio: 0 at the start, about 116 at the end of the first rail; about 0.9 per second) | same |
| `ride_end` | a ride was seen and the rail has not moved for 90 presents | same |
| `film<=<n>` | the course HUD film counter (72 at the start) is at most n (`film<=0`: the film is used up, which ends the expedition) | existing hook `rt_gm_film_c` at `main+0x393d9c` (`neofilm.cpp`): HUD `+0x2b8` |
| `focus=<element>` | the newest UI focus event (Lumen element name, e.g. photo select's `AllCheck` = the Auto toggle) named that element | `route_ui` event 16 (`runtime\hle\input_core.cpp`) |

Conditional presses (2026-11-14, QA sweep): any token line may carry `if=<condition>` (a wait condition, `!` negates); it presses only while the condition holds. Photo select's Auto toggle sits one or two Downs below the folders depending on the folder count, so `repeat @f100 @f900 90 8 Down if=!focus=AllCheck` then `A` works for both (`analysis\core	ail_auto2.txt`).

Course choice (2026-11-18): the line `course <name>` (`florio`, `jungle`, `forest`, `sands`, `river`, `beach`, `reef`, `volcano`, `ruins`, `cave`, `snow`) moves the stage-select map to that course and opens its course list (`course_steps` in `runtime\hle\input_core.cpp`, test `course_steps`). The map's focus element is the stage number (Florio 1, beach 3, jungle 5, forest 6, sands 7, volcano 8, snow 9, cave 10, ruins 11, river 13, reef 14), and each D-pad press repeats only while the stage before it has focus. Fixed-time presses had landed on three different courses in three runs, and one cause was found in the test setup: hidden route runs take input in the background (`SNAP_INPUT_BACKGROUND=1`) and XInput/SDL pads are global devices, so a controller in use elsewhere on the PC pressed buttons in the run (run `0-20261118_132242`: focus on photo select moved between folders, Auto and Delete with no route press). A scripted background run now ignores the real pads and logs `pad: real pad input ignored ...` when one is active (`script_run_drops_pads`, `runtime\hle\input_core.h`; `SNAP_INPUT_REAL_PADS=1` keeps them). A visible game is unchanged. An unknown name or a map that opens off the path ends the run with exit code 3.

Game-time rides (2026-11-13): present counts swing about 7 times with PC load, while the rail and camera turns follow game time. Chain `wait rail>=<x>` (alias of `ride>=<x>`) for rail points and use seconds lines (`<start> <hold> tokens`, no `@f`) inside each segment; both stay put under load. Keep `@f` only on `shot=` lines (a `shot=` on a seconds line dumped no frames in run `0-20261113_235715`).

Gotcha: when a `wait` (or `anchor`) is met, the lines of the segment before it stop for good. A line whose start time (for example `@f240`) lies after the moment the wait was met never fires. The runner now logs `input script: WARNING segment <k> ended before <n> of its lines started (first at @f<t>); they never fire` at that point (2026-11-13). Put such lines after the wait, timed from it, or keep the whole sequence before the wait.

Debug: `SNAP_ROUTE_LOG=1` logs every scene, state change, non-frame UI event (1 built, 2 opened, 3 closed, 5/6 state entered/ready,
7 input on, 16 focus, 18+10 decide, 21+12 decide done), rail start/move/restart, film count and dialog window (`route:` lines in
`boot_stderr.log`); read it to find the state names for a new route. All of it is off (each hook returns at once) unless a script has a
`wait` line or `SNAP_ROUTE_LOG` is set. Routes using it: `florio_eval`, `florio_photos`, `post_tutorial_check`, `unlock_all_check`,
template `core_template.txt` for the course sweep.

Fixture `story_complete` (2026-11-13, secrets worker): `unlock_all` plus every scenario bit (file 9 entry 0 offset 0x3b58 = 0xFFFFFFFFFFF, events 0..43) and all Boss flag bytes (0x3b88..0x3b9b = 1), built with `scripts\snap_save.py patch` (flags per `analysis\core\story.md`). Use it for content gated on story completion (Florio Secret Side Path, Mew). `analysis\core\secrets\run_s.sh` takes it through `SNAP_FIX=story_complete`. Route token `shot=<n>` (in a `@f<start> <hold> shot=<n>` line) dumps a 4-frame burst every n presents.

### Parallel runs (owner exception to the one-heavy-process rule)

Locks (`host\locks.ps1`; a lock file holds `owner pid=<pid> <time> <what>`, stale when the pid is dead or it is older than 20 min):

- **Build lock** `build\heavy.lock`: one build (`host\build_snap.py`, `snap_build`) at a time. Unchanged.
- **Run slots** `build\slots\run0..3.lock`: `host\run_boot.ps1` takes one (up to 4 hidden runs at once; owner 2026-11-12, cut from 6 because 6 held the CPU at 100%), waits and prints the owners when full,
  releases in a finally block. While `build\heavy.priority` exists slot 0 is only for the run with `$env:SNAP_LOCK_OWNER` equal to its content.
- **Muted**: hidden runs (`run_boot.ps1`, `run_boot_mods.ps1` without `-Visible`) default to `SNAP_AUDIO_BACKEND=null`: no sound reaches the speakers, while audio timing and `SNAP_AUDIO_CAPTURE` work as before. Set `SNAP_AUDIO_BACKEND=wasapi` only for a test that needs the real device (spatial or WASAPI channel checks), and then mute the game with `SNAP_VOL_MASTER=0` unless the sound itself is under test.
- **Own builds**: `host\build_snap.py --out build\w\<owner>` relinks into a private folder without `build\heavy.lock`; run it with `run_boot.ps1 -BuildDir build\w\<owner>` (or `$env:SNAP_BUILD_DIR`). A build dir without its own `nvn_cache` gets the warm one from `build\snap`. Take `heavy.lock` only to write `build\snap`, rebuild the main/sdk libs (`recomp\snap_build.py`) or regenerate main.
- **Exclusive**: `-Bench`, `-Exclusive` or `SNAP_PERF` set: takes `build\slots\exclusive.lock`, then all 4 slots; new normal runs wait behind it.
- **Shared perf runs** (2026-11-13): `SNAP_PERF_SHARED=<prefix>` writes the same `SNAP_PERF` CSVs but `run_boot.ps1` does not key on it, so the run takes one normal slot. For A/B comparisons on a shared PC only (alternate A and B in the same time window); absolute numbers are not comparable with exclusive benches.

Each run copies `build\snap\snap_boot.exe`, DLLs and `nvn_cache` to `build\runs\slot<N>\` (a concurrent build may relink), uses
`build\runs\slot<N>\save` and its own copy of the shared `%APPDATA%\SnapRecomp\shadercache` (via `SNAP_SHADERCACHE_DIR`; warm, no concurrent
writes; new pipelines found by a run are not merged back), and runs at BelowNormal. Output has a run id `<slot>-<yyyymmdd_hhmmss>`:
`analysis\boot\runs\<id>\boot_trace.log` (+ `boot_stderr.log`, `boot_stdout.log`), `route_<name>_<id>_NN.png` or `run_<id>_NN.png`
(frame dumps; `-ShotAt` without `-Route` also dumps). Old names stay as copies of the latest finished run: `analysis\boot\boot_trace.log`,
`boot_stderr.log`, `boot_stdout.log`, `route_<name>_NN.png`. An explicit `-Log` is honored as-is (stderr/stdout beside it). `build\testsave`
is no longer used. For other scripts: `. host\locks.ps1; Take-BuildLock 'what'; ...; Release-Lock build\heavy.lock`.

### Scripted routes (`-Route`)

    powershell -File host/run_boot.ps1 -Route florio_photos [-Timeout 295] [-ShotAt 40] [-ShotEvery 15] [-ShotMax 17] [-SaveFixture name] [-NoLock]

Feeds `host\routes\<name>.txt` as `SNAP_INPUT_SCRIPT` (format: see Scripted input below), seeds the run's save dir from the fixture named
on the file's first line (`# fixture: post_tutorial`; `-SaveFixture` overrides), takes a run slot (see Parallel runs; `-NoLock` if the
caller holds one) and writes checkpoint screenshots `analysis\boot\route_<name>_<runid>_NN.png` (latest also copied to `route_<name>_NN.png`) (half size, from `NVN_FRAME_DUMP`; the desktop capture
`SNAP_SHOT` is black on the hidden desktop). Use `-ShotAt` 40 or lower: with a later start no dumps were produced in some runs. Script time is
wall time from the first input poll, so a loaded PC shifts the game but not the script (use frame times, below); presses during loading are lost.
`florio_photos` (A taps plus ZR) reaches the lab intro and the camera tutorial by ~190 s; later steps are unverified (see
`analysis\recomp\integration_issues.md`). `host\routes\unlock_all.txt` is the same A-tap probe on the `unlock_all` fixture.

## PC tabs in the game's own Settings screen (2026-11-13, `host/mods/nativeopt.cpp`)

The game's Settings screen (lab hub gear) is data driven: `option.lmb` in `common/UI/master/Scenes/option.drp` lists states
(tabs), elements and focus groups (format and dumper: `scripts/ui/lmb.py dump <option.lmb> [state]`). The PC settings are thirteen
extra tabs in sets of five after the game's "Other" tab (R past Other; L on Game goes to the last one): Quality, Textures,
Effects, Output, View | Display, Controller, Mouse, Audio, Photos | Shadows & AO, Performance (overlay mode and corner, see
"Performance overlay"), Upscaler (the two upscaler rows, moved off Quality when the Look row took its first slot). They use the game's own row widgets (two-choice rows,
pulldowns with their popup), tab bar, help line, sounds and Restore Default Settings button.

- Pack: `python -I scripts/ui/make_native_options.py [--src <option.lmb>] [--out <mods root>] [--if-stale]` writes the local
  mod pack `native-options` (patched `option.lmb`, `rows.txt`, `pack.json`; default out `%APPDATA%\SnapRecomp\mods`) from
  the user's own extracted `option.lmb`. It is game-derived: never commit it. `host\play.ps1` runs it with `--if-stale`
  before every start (rebuilds when the pack is missing or the script changed; `-NoNativeOptions` skips it; a failure only
  means no PC tabs). Rows are the `TABS` table in the script; `rows.txt` carries them to the host, so there is no second list.
- State switches are the Lumen player's own: an element whose f2 names a state (f6 = 1) goes there on decide (tabs, pulldown
  rows), f6 = 4 returns to the previous state (popup items), and L/R open the state named at +0x30 of the page's Prev/Next
  element entries (BuildPage fills in the neighbor game tabs). The player then sends event 5 with the new state; for a PC
  state `rt_ui_opt_event` (`0x728e10`) builds the page with the game's BuildPage (`0x737360`), renames it and its five tab
  entries, points Prev/Next at the neighbor PC tabs (Other before PC tab 1, Game after the last), adds the PC variables and
  row element entries (the cursor only reaches listed elements) and calls SetState. Calling SetState at any other time does
  not switch states. After SetState the hook redraws each pulldown row's value text (the player keeps the old text). A
  two-choice element's focus action leaves its box at `lb_select` (no unfocus action; the game's handler repaints), so the
  hook repaints a two-choice row from its value when the cursor leaves it; without that both boxes stayed yellow. A popup
  is the game's own list element (`PulldownMenu`, type 6, f4 = last item): it moves the cursor itself and reports
  `PulldownMenuItemNN` (separate item elements linked by focus groups never moved the cursor). As the game's list helper
  does (`0x776c80`, `0x776bb0`), on decide on the row (before the player switches) the hook registers the items with
  `player->vtable[0x148](player, &popup state, &"PulldownMenuItemNN", true)`, focuses the current value
  (`player->vtable[0x1f0]`), and starts the list itself (goto + play `lb_start_listNN`, NN = items + 1, on the popup's
  `pulldown_NN_mc` through `vtable[0x90]`/`[0x98]`, as `0x776980` does), then focuses the current value again; the popup
  states have no `lb_start_list` enter action (the game's do not either). So a popup opens with the cursor (the game's
  yellow highlight, its only marking of the current value) on the current value (verified 2026-11-14: Render scale opens
  on 1x, Shadow distance on 1x). A popup grows downward from its row, so a long list on a low row opens from a higher
  row's popup (`pull_slot` in the script, column 9 of `rows.txt`; row 1 holds 10 items). Limit: Sharpening and Master
  volume are pulldowns, not the game's 0..10 slider: its knob is moved by the game's own tab handler, which a PC page
  bypasses.
- Tab sets: the screen has 2- to 5-tab bars (`window_mc` labels `lb_tab_02..05`, chosen by `{TabNum}`; the game plays it
  only in Init). Every game and PC tab state replays `lb_tab_0{TabNum}`, and for a short last set (Shadows & AO alone)
  the hook overwrites `TabNum` in the descriptor's variable map with max(2, tabs in the set); a spare slot is blank.
  The generator asserts at most 6 rows per tab (no scrolling), 10 items per pulldown, 112-character help.
- "(env)" after a pulldown's value: the row's `SNAP_*` variable was set by hand before start, so it wins over the saved
  value (`settings::env_wins`; the F1 menu shows the same warning).
- Visual extras (owner 2026-11-15: new installs start on the Enhanced look; the Look row, first on the Quality tab, flips them all, and its
  Restore Default Settings goes back to Enhanced): Anti-aliasing (stores the F1 table's values, so the quality preset keeps its name:
  `default` follows the Look, `off` is the game's own FXAA, `smaa`, `ssaa2,smaa`), Sharpening (Off, 0.1 to 0.7, 0.85, 1.0), Texture filtering
  (Original / 2x / 4x / 8x / 16x), Sharper menu text (Off = `off` / On = `auto`, 2x at 1440p and above) and Save PNG
  copies. Their defaults (and so Restore Default Settings) are read from `runtime\hle\visual_defaults.h` when the pack is
  built (the pack rebuilds when that file changes); they apply after a restart (the F1 table marks them `kRestart`).
- Advanced rows (2026-11-14, `docs/advanced-graphics-options.md`; all default to the original look): Render scale adds
  1.75x (10 steps, 0.5x to 4x), Texture sharpness -2 to +2 (10 steps), Frame rate limit adds 75, 165 and 240; Output: HDR
  output Off / HDR10 (`1`, what the old on/off row stored) / scRGB (`scrgb`), HDR peak Auto (the key erased: the
  monitor's reported peak, else 1000) or 400 to 4000 nits, HDR highlights (`gfx_hdr_expand` -> `SNAP_HDR_EXPAND`, 0 to 1,
  default 0.3); tab Shadows & AO: Shadow distance (`gfx_shadow_dist` -> `SNAP_SHADOW_DISTANCE`, 1x to 16x; the quality
  presets set it with the shadow resolution: Original 1x, High 2x, Ultra 4x, Maximum 8x, so shadow texels per meter stay
  the game's), Contact shadows (`contact_shadows` -> `SNAP_CONTACT_SHADOWS`, Off / On, default Off, live through
  `snap_contact_shadows_set` in `runtime/nvn/nvn_contact.cpp`). The quality preset that is all defaults (id `normal`) is
  labeled Original; Render scale 4x reads "4x (8K, heavy VRAM)". Sharpening and Master volume stay the game's own
  pulldown popup (no slider knob on PC pages). Reserved, not drawn yet: AO radius and strength on Shadows & AO (`SNAP_SSAO_RADIUS` /
  `SNAP_SSAO_STRENGTH` are parsed in `host/mods/gamemods.cpp` but never written to the pass), the lighting stream's rows
  after them. Vsync (Output row 6 and F1 Graphics "Frame rate"): On / Adaptive / Fast / Off, live through
  `snap_vsync_set`, remembered as settings key `vsync` (exported as `SNAP_VSYNC` at the next start; On erases it).
- `rt_ui_opt_state` (the 8 SetState `blr`s) keeps a template element entry and points the game's Other page's Next at PC tab
  1 and the Game page's Prev at the last PC tab. Choices and popup picks are stored where the
  F1 overlay stores them (`snap::ui::options_test_set`: same keys, defaults, custom stores and presets; the controls mapping;
  the master volume). "Applies after restart." is in the help line of rows that need it.
- Restore Default Settings (Minus) on a PC tab resets that tab's rows to their defaults and redraws them; on a game tab it
  resets only that game tab (the game's own behavior, which never touches PC settings). Each PC tab's help line says so.
- Not in the PC tabs (F1 menu only): PNG folder path, lighting preset, stick dead zones, mouse smoothing, F1 menu scale,
  bindings, mods, saves, test input.
- `SNAP_NATIVE_OPT_TRACE=1` logs events, states and element lists. `SNAP_NATIVE_OPT_SETTINGS=<file>` is a test aid: runs
  with `SNAP_SAVE_DIR` skip `settings::load()`, so the hook reads and writes this settings file instead. Set
  `SNAP_CONFIG_DIR` to a private folder in test runs too, or Controller/Mouse/Display changes write the user's
  `%APPDATA%` input/window/audio files.
- Route `host\routes\native_options_tabs.txt` (`post_tutorial`): opens Settings, R to PC tab 1, then visits every PC tab with
  one change each (pulldown picks with Down or Up from the current value inside the popup, choices), Minus on Photos, B
  (leaves the Reset button), R to Shadows & AO, Shadow distance 1.5x and Contact shadows On, R to Performance, overlay FPS then Detailed, R (wraps to Game), L (back to Performance), B closes (Detailed stays on over the hub; run with `NVN_FRAME_DUMP_POST=1` to see it in the shots). Run it with
  `SNAP_MODS_DIR=build\w\<owner>_mods` (pack built with `--out` there) plus the three switches above.

## Visual defaults, the Look row and SNAP_VISUALS (2026-11-13, Enhanced by default since 2026-11-15, `runtime/hle/visual_defaults.h`)

The settings whose defaults differ from the original game live in one table. The owner chose the original look as the default on 2026-11-13 and the Enhanced look for new installs on 2026-11-15 (`snapdef::kDefaultEnhanced = true`). Each extra still works on its own through its own switch (an F1 menu choice, which `settings.cpp` exports as env, or a hand-set env var), and that always wins over the profile and the Look. `SNAP_VISUALS=enhanced` or `original` forces every row's column for one run. A player's saved choices are never rewritten: only keys that were never saved (or saved as `default`, which is the same as unset) take the new default.

**Look row.** `Look: Enhanced / Original` is the first row of the F1 Graphics page and of the in-game Settings Quality tab. It stores settings key `gfx_look` (`original`; Enhanced is the erased default), which `settings.cpp` hands to `snapdef::saved_look()` (order of precedence: `SNAP_VISUALS` env, saved Look, `kDefaultEnhanced`). Picking either value also erases the rows' own keys (`snapdef::look_keys()`: `gfx_aa`, `gfx_sharpen`, `gfx_aniso`, `gfx_ui_font`, `photo_export`, `hd_photos`) so the whole bundle follows the Look; rows changed afterwards keep their own value. Restore Default Settings on the Quality tab resets the Look to Enhanced with the other rows. The row takes effect at the next start. The launcher can bind the same key (S_CFG `gfx_look`, then erase `look_keys()`). **Hook for Lighting v2:** when its Enhanced lighting passes the photo-score and 144 fps checks, add the commented `SNAP_LIGHTING` row of the table (original `original`, enhanced `enhanced`), make `lighting_preset.h` read it with `snapdef::get`, and add `gfx_lighting` to `look_keys()`; nothing else changes. Tests: `tests/hle/hle_tests.cpp` (settings: saved keys kept, Look column, `SNAP_VISUALS` wins), `tests/hle/ui_look_script.txt` (F1 row: starts Enhanced, flips and clears together, flips back; run with `SNAP_UI_TEST`, no `SNAP_VISUALS`), `host/routes/look_native_opt.txt` (the in-game Quality tab: Look to Original, Restore Default back to Enhanced).

| Setting (env) | Original (`SNAP_VISUALS=original`) | Enhanced (default) | Read by |
|---|---|---|---|
| Anti-aliasing (`SNAP_AA`) | `original`: the game's own FXAA | `ssaa2+smaa`: render height = 2x the window height up to a budget of 2160 lines (2x at 1080p, 1.5x at 1440p, 1x at 4K), plus SMAA | `nvn_aa.cpp` (`AaParseEnv`) |
| Sharpening (`SNAP_SHARPEN`) | 0 (off) | 0.30 (CAS) | `nvn_aa.cpp`; off with an upscaler unless `SNAP_SHARPEN` is set |
| SMAA quality (`SNAP_SMAA_QUALITY`, key `gfx_smaa_quality`) | `high` | `high` | `nvn_aa.cpp` (`SmaaPreset`): the reference SMAA presets as push constants. Low 0.15 / 4 steps / no diagonals or corners, Medium 0.1 / 8 / none, High 0.1 / 16 / 8 diagonal / corners 25, Ultra 0.05 / 32 / 16 / 25. Only SMAA reads it; the same value in the launcher, F1 and the game's Settings |
| Anisotropic filtering (`SNAP_ANISO`) | 0: the game's own filtering | 16x | `nvn_entry.cpp` (`ApplyHostEnvPolicy`) |
| UI font supersampling (`SNAP_UI_FONT_SCALE`) | `off` | `auto`: 2x at a 1440p+ window | `gamemods.cpp` (`decide_font_scale`) |
| PNG photo export (`SNAP_PHOTO_EXPORT`) | 0 (off) | 1 (on) | `photoexport.cpp` |
| HD photos (`SNAP_HD_PHOTOS`) | 0 (off) | 1 (on) | `nvn_hdphoto.cpp` (`HdPhotoOn`, read once) |

`SNAP_LOD_BIAS` is not a row: its automatic mode only follows a render scale above 1 and does nothing at 1x, so both profiles behave the same. The ssaa2 scale depends on who chose it. The Enhanced default (no `SNAP_AA` set) renders min(2x the window height, 2160 lines) over 1080, so the cost follows the output: 1080p renders 2160 lines (2x per side), 1440p 2160 (1.5x), 2160p 2160 (1.0x, with SMAA and sharpening); `SNAP_SSAA_BUDGET_H=<lines>` changes the budget and `0` removes it (the old 2x of the window); the log line `aa: ssaa2 auto: ...` shows the result. A player-chosen `SNAP_AA=ssaa2...` (the High, Ultra and Maximum presets, or the AA row) keeps the full 2x of the window height, capped at 3.0 (5760 x 3240) and 8192 px per side, so a 2160p window renders at 3.0, not 4.0 (log line `aa: ssaa2 scale capped ...`). An explicit `SNAP_RENDER_SCALE` is never capped. The window height used for this (and by the upscaler) is `SNAP_WINDOW_H`, else the primary monitor height when the window is borderless or maximized (`SNAP_WINDOW_MODE` or `window.json`; read once at start, so a later F11 does not change it), else the `window.json` windowed height, else 720. With no `mods.json`, no mod pack is enabled except `native-options` (the PC tabs in the game's Settings screen); `mods.json` can still disable it.

## Detail preset (F1 > Graphics > Detail preset)

The Detail preset row (key `gfx_preset`, called Quality preset before the Graphics preset row arrived; see `docs/presets.md` for that row) picks Very Low, Low, Default, High, Ultra or Maximum and sets nine rows at once (Default erases them, so they follow the Look); it shows Custom when the rows match no preset (the preset itself is not stored). All rows need a restart. Blank = the row's default.

| Row (env) | Very Low | Low | Default | High | Ultra | Maximum |
|---|---|---|---|---|---|---|
| Render scale (`SNAP_RENDER_SCALE`) | 0.50 | 1 | default | default | 2.00 | 4.00 |
| Anti-aliasing (`SNAP_AA`) | none | off (game FXAA) | default (ssaa2+smaa; the game's own FXAA with the Original look) | ssaa2,smaa | smaa | smaa |
| Sharpening (`SNAP_SHARPEN`) | 0 | 0 | default | 0.30 | 0.30 | 0.50 |
| Aniso (`SNAP_ANISO`) | off | 4 | default (16; off with the Original look) | 16 | 16 | 16 |
| Texture sharpness (`SNAP_LOD_BIAS`) | +1 | auto | auto | auto | auto | auto |
| Shadow map (`SNAP_SHADOW_RES`) | 512 | 1024 | 1024 | 2048 | 4096 | 8192 |
| AO quality (`SNAP_SSAO_QUALITY`) | default | default | default | high | ultra | ultra |
| AO samples (`SNAP_SSAO_SAMPLES`, new row) | 4 | 8 | default | default | default | 512 |
| HBAO dirs x steps (`SNAP_SSAO_HBAO`, new row) | default | default | default | default | default | 32 x 128 |

Left out on purpose: detail distance (display-only, but it costs speed), always full detail, draw distance, full-rate animation (photo scores unverified), FOV (paused), lighting preset (lighting worker), HDR, color look, frame cap. Upscaler and upscaler quality rows are now live (restart) but in no preset. The upscaler replaces the render scale and the ssaa2 default (the game's FXAA is off unless `SNAP_AA` is set); its scene scale is below 1 in a window under 1080p (works since the fix below; DLSS Quality at 1280x720 scored in run `0-20261112_174810`).

Findings (2026-11-12, florio_eval / florio_photos, hidden 1280x720 window, RTX 4090, 2 to 4 runs at once with other workers, so absolute times are noisy):
- Sub-native render scale lost the GPU device (`vkQueueSubmit2 failed: -4`) 20 to 30 s after the score screen in 4 of 4 runs at 0.5/0.75. Cause: the album thumbnail readback copied a 240x200 guest region out of the 120x100 scaled image. Fixed 2026-11-12 in `runtime\nvn\nvn_xfer.cpp` (render-scale-aware copies, `NVN_NO_XFER_SCALE=1` restores the old ones; runs in `AGENTS.md`, "Sub-native scale device loss fixed"). Very Low now draws at 0.5 (960 x 540) and the slider starts at 0.5. 0.5 and 0.75 each scored in 2 runs and kept running 160 to 200 s past the score screen with no device loss.
- Scale 6 and 8 crash the guest before the title (run `0-20261112_144057`); 4 works.
- `SNAP_SHADOW_RES=512` is new (the depth maps scale 0.5). `SNAP_PASS_OFF` works since 2026-11-12 (see "Render pass toggles" below).
- Presents are paced to 60 Hz, so ms/frame below is mostly the limiter plus CPU load, not GPU cost: Normal 18.9 (`3-20261112_131027`), Very Low 17.7 (`0-20261112_141432`, florio_eval, photo scored, 2,375 points shown), Low 17.6 (`1-20261112_141432`), High 18.1 (`0-20261112_142450`), Ultra 18.4 (`1-20261112_142450`), Maximum 19.5 (`0-20261112_143210`, 7680x4320 scene), Maximum with a 3840x2160 window 19.7 (`0-20261112_144203`). Maximum does not lag a 4090 under this pacing; an exclusive `SNAP_PERF` bench is needed for real GPU cost.
- AO samples and shadow size do not change the course picture (lighting worker: no SSAO in the course, shadows mostly baked).
- Not done: the one-time preset suggestion from the GPU name; scoring on High, Ultra, Maximum (only Very Low and Low ran `florio_eval`; Normal at 1x/ssaa default scored in earlier runs); the F1 test `tests\hle\run_options_test.ps1` passes the new preset checks (25 checks, FOV expectations removed).

## Fx: host screen-space effects (2026-11-14, `runtime/nvn/nvn_fx.{h,cpp}`, effects in their own files)

All off by default; developer switches only (no options rows yet; rows wanted are in the Fx worker report in `AGENTS.md`).

| Switch | Meaning |
|---|---|
| `SNAP_FX=0` | Forces every Fx effect off. Unset or 1: an effect runs only when its own switch is on. |
| `SNAP_FX_AB=<P>` | Effects run only while (present count / P) is odd: same-view A/B in one run (dump frames N and N+1 at a block edge). |
| `SNAP_FX_SPLIT=1` | The left half of the frame keeps the game's own picture, the right half gets the effects (one-frame A/B). |
| `SNAP_FX_PHOTO_HOLD=1` | Effects also switch off for 3 presents at every photo taken on a ride (the behaviour before 2026-11-19). Default 0: only the Evaluation passes (photo select, scoring) switch them off. |
| `SNAP_RT_DEMAND=0` | The ray tracing scene capture runs every frame again. Default: it pauses after 120 presents in which no reflection, bounce light or AO pass traced the scene (no water in view, Bounce light off) and resumes on the next pass that needs it. Saves 2 to 4 ms of GPU time per frame at 2880x1620 while paused. |
| `SNAP_LIGHT_HALF=0` | Enhanced lighting works out AO and contact shadows for every pixel each frame again. Default: one pixel of every 2x2 block per frame, spread by depth and accumulated (about 1 ms less GPU time at 2880x1620). |
| `SNAP_LIVE_SIZE=0` | The Internal resolution, Anti-aliasing, Shadow resolution and Ultrawide rows need a restart again. Default: a change makes the affected render targets again at the next frame (`runtime/nvn/nvn_retarget.cpp`; about 20 to 40 ms for that frame at 4K). Still restart-only: switching an upscaler or frame generation on or off, and switching to or from the game's own FXAA. |
| `SNAP_HDPHOTO_PLAIN_COPY=1` | HD photo captures are read back with a plain memory copy again (60 to 80 ms on the game thread per shot; the default streaming copy takes about 5 ms). |
| `SNAP_CONTACT_SKY_WC=1` | The contact shadow sky check reads its depth rows from write-combined GPU memory again (10 to 14 ms of one frame in every 20 at 2880x1620). Default: cached host memory, 0.2 ms. `SNAP_FX_LOG=1` prints the time of each count. |
| `SNAP_FX_SHUTTER_DUMP=<n>` | Test aid, needs `NVN_FRAME_DUMP`: dumps every present of the n after an A press on a ride (first 4 presses, 240 presents apart). |
| `SNAP_FX_LOG=1` | Logs frame phases, camera, shadow caster capture, photo holds and effect runs (`fx:` lines on stderr). |
| `SNAP_FX_TIME=1` | GPU time of the whole pre-tonemap Fx block (timestamps; avg/max logged every 300 blocks). |
| `SNAP_CONTACT_SHADOWS=1` | Screen-space contact shadows (`nvn_contact.cpp`, `shaders/contact.frag`). Tuning (test aids): `SNAP_CONTACT_STRENGTH` (0.5), `SNAP_CONTACT_LEN` (0.4 world units), `SNAP_CONTACT_THICK` (0.15), `SNAP_CONTACT_FADE` (45 view units), `SNAP_CONTACT_LSIGN` (+1/-1 forces the light direction sign), `SNAP_CONTACT_DEBUG` (2 tints, 3 view distance, 4 normal, 5 sun facing, 6 depth, 7 the factor itself, 100 cycles). |
| `SNAP_FOLIAGE_AA=1` | Leaf edge smoothing (`nvn_foliage.cpp`, `docs/foliage-effects.md`): alpha-tested scene draws dither their edge (hashed alpha) instead of a hard discard; photo frames keep the game's own draw. Live in the dev loop (`dev_param`). |
| `SNAP_LEAF_TRANSLUCENCY=0.5\|1` | Leaf translucency: back-lit glow on alpha-tested lit materials when the camera looks toward the sun. Test aids: `SNAP_LEAF_DEBUG=1` (paints the term red across the whole d range), `SNAP_LEAF_FLIP=1` (uses the opposite sign of the light direction). |
| `SNAP_WIND_SWAY=0.5\|1` | Wind sway (`nvn_foliage.cpp`): vertex wrapper on alpha-tested scene draws adds a small horizontal world-space sway driven by the absolute timer; photo frames keep the game's own vertex shader. Experimental: no menu row yet, swayed leaves still render dark (`docs/foliage-effects.md` section 4). Restart only. Test aids: `SNAP_WIND_STATIC=1` (freeze the field), `SNAP_WIND_AMP` (world units at full strength, default 0.05), `SNAP_WIND_PHASE` 0..7 (time offset, so a frozen frame can show motion), `SNAP_FOLIAGE_PICK` 0..4 (dev loop: 1 = smoothing, 2 = translucency, 3 = sway, 4 = all, for cost runs). |
| `SNAP_SOFT_PARTICLES=1` | Draws the half-resolution particle layer (960x540 colour + depth, creation sites in `FoliageParticleBoost`) at the scene's resolution. Restart only (targets are sized at creation). `SNAP_SOFT_PARTICLES_SEL` bitmask (default 6) picks which of the three targets A/B/C to boost. |

Regenerate the host SPIR-V headers (`nvn_fx_spv.h`, `nvn_contact_spv.h`) after editing a shader: `tools\venv-recomp\Scripts\python -I runtime\nvn\shaders\make_fx_spv.py`.

## Lighting switches (env, or the options rows that set them)

| Switch | Effect |
| --- | --- |
| `SNAP_LIGHTING=enhanced\|maximum` | Enhanced lighting preset (menu row "Lighting preset", launcher row "Lighting", default Original). Enhanced: PCSS soft shadows (16 taps, contact hardening), shadow map sized from the render scale (smallest power of two at or above 1080 x scale, 2048 to 8192), 2x shadow distance, GTAO ambient occlusion with bilateral and temporal denoise, clean contact shadows (`nvn_lighting_fx.cpp`). Maximum: one step larger shadow map, 4x shadow distance, 3 AO slices. `high` and `ultra` are aliases of the two. Explicit switches win. The extra passes skip photo frames (`FxIsPhotoFrame`), so scoring is untouched. |
| `SNAP_SHADOW_PCSS=1` | PCSS on its own (blocker search plus penumbra scaling in the receiver shader, `nvn_shadowpcf.cpp`). Tuning: `SNAP_SHADOW_PCSS_LIGHT` (2.5), `_MAX` (3.5 texels, about 3 to 4 times the Original PCF radius), `_SEARCH` (8 taps), `_FLIP`. `SNAP_SHADOW_FILTER` sets the PCF taps (16 with PCSS). |
| `SNAP_LIGHT_FX=1` | The AO and contact-shadow passes on their own. Tuning: `SNAP_LIGHT_AO=0` / `SNAP_LIGHT_CONTACT=0` (turn a half off), `SNAP_LIGHT_AO_RADIUS` (1.0), `_AO_STRENGTH` (0.85), `_AO_POWER` (1.6), `SNAP_LIGHT_CONTACT_STRENGTH` (0.6), `_CONTACT_LEN` (0.5), `_CONTACT_THICK` (0.2), `SNAP_LIGHT_FADE` (60 view units), `SNAP_LIGHT_ALPHA` (0.1 temporal weight), `SNAP_LIGHT_DEBUG` (1 AO, 2 contact, 3 raw AO), `SNAP_LIGHT_AB=<P>` (effects on in odd presents only, for same-view A/B: `scripts/light/run_light_compare.ps1`, `pair_light.py`). |
| `SNAP_SHADOW_DISTANCE=<1..16>`, `SNAP_SHADOW_FADE=<scale>` | Widen the dynamic shadow map k times (casters scaled, receivers remapped in shader); fade length and far clip scaled by the same k. |
| `SNAP_SHADOW_FILTER=4\|8\|16\|32`, `SNAP_SHADOW_SOFTNESS`, `SNAP_SHADOW_BIAS` | Soft shadow taps (PCF), kernel radius in 1024-map texels, reference-depth bias in 1e-4 units. |
| `SNAP_SSAO_FORCE=1\|2` | Turn the SSAO passes on in the course (1 strict, 2 relaxed). Experimental: draws nothing yet. |
| `SNAP_SSAO_GTAO=1`, `SNAP_GTAO_KEY=<hex>` | Replace the HBAO fragment shader with GTAO. Experimental, needs the HBAO key. |
| `SNAP_LIGHT_AB=<P>`, `NVN_P1_LOG=1` | Test aids: toggle the features every P presents; log PerViewCBuffer binds. |
| `SNAP_BOKEH_DOF=<s>`, `SNAP_MOTION_BLUR=<s>` | Lens effects (2026-11-15, `runtime/nvn/nvn_lens_fx.cpp`, `docs/lens-effects.md`; menu tab 15 "Lens", rows "Zoom bokeh" 0.6 / 1 / 1.6 and "Motion blur" 0.5 / 1 / 2; default off, restart). Display only: pre-tonemap Fx effects, never run on photo frames. Bokeh: round background blur while ZL zooms (ride 36 degrees, zoomed 14.44 degrees), focus = median depth under the reticle. Motion blur: camera motion applied to the depth buffer. Test aids: `SNAP_BOKEH_FORCE=<zoom>`, `SNAP_BOKEH_BASE_FOV`, `SNAP_BOKEH_DEBUG=1`, `SNAP_MBLUR_DEBUG=1`, `SNAP_LENS_LOG=1`, `SNAP_LENS_TIME=1` (GPU ms per effect); `SNAP_LIGHT_AB` toggles them too. Proof scripts: `scripts/lens/`, route `lens_zoom`. |
| `SNAP_ATMOS_SHAFTS=1`, `SNAP_ATMOS_FOG=1`, `SNAP_ATMOS_EXPOSURE=1`, `SNAP_ATMOS_BLOOM=1`, `SNAP_ATMOS_CLOUDS=1`, `SNAP_ATMOS_SKY=1`, `SNAP_ATMOS=1` (all six) | Atmosphere effects (2026-11-15, `runtime/nvn/nvn_atmos.cpp`, `docs/atmosphere.md`; menu tab 16 "Atmosphere"; default off, restart). Display only: pre-tonemap Fx effects on the HDR scene colour, never run on photo frames. Tunables are `SNAP_ATMOS_<EFFECT>_*` dev params (F1 Tuning page with `SNAP_DEV=1`); `SNAP_ATMOS_DEBUG=1\|3\|4\|5\|6` shows one term alone. |
| `SNAP_WATER_SSR=1`, `SNAP_WATER_CAUSTICS=1`, `SNAP_WATER_FOAM=1`, `SNAP_WATER_OCEAN=1` | Water effects (2026-11-16, `runtime/nvn/nvn_ssr.cpp`, `shaders/water_fx.frag`, `docs/water-effects.md`; menu tab 17 "Water", rows "Water reflections", "Water caustics", "Shoreline foam", "Ocean waves" (`SNAP_WATER_OCEAN`: the FFT ocean of `runtime/fx/ocean_fft`, wired by `runtime/nvn/nvn_ocean.cpp`; sets the reflection normals and draws whitecaps; `SNAP_WATER_OCEAN_STRENGTH`); default off, Live). Display only: one pass over the scene colour after the game composites its half-resolution sea, never on photo frames. Reflections march the scene depth and fall back to the game's cubemap on a miss and at the screen edges. Test aids: `SNAP_WATER_DEBUG=1\|2` (water mask / height above the floor), `SNAP_WATER_LOG=1`, `SNAP_WATER_TIME=1` (GPU ms), `SNAP_WATER_FS=<key,...>` (extra water programs), `SNAP_WATER_SSR_STRENGTH`, `SNAP_WATER_SSR_DIST`, `SNAP_WATER_RIPPLE`, `SNAP_WATER_CAUSTICS_STRENGTH`, `SNAP_WATER_FOAM_STRENGTH`; `SNAP_LIGHT_AB` toggles them too. `SNAP_DRAW_SKIP_GROUPS=<keys;keys;...>` skips one fragment-key group per 4-present block (same-run A/B to find which draws make a surface). |

## Layers and registration order (`bind_host_layers` in `boot.cpp`)

1. `hle::bind_all`: real HLE overrides are strong; the generated not-implemented stubs are weak (bound only when no module defines the symbol).
2. `snap_bind_audio()` (`abi_audio.cpp`): the 16 `nn::audio` overrides, host C ABI to guest ABI.
3. `snap_bind_nvn()`: the `nvnBootstrapLoader` override; `nvnDeviceGetProcAddress` and the table hand out thunks.
4. `os_override_table()`: the whole os layer, one loop. To swap the os layer, change only that loop and the `os_*` calls in `run_thread`
   and `cmd_run`.

Symbol precedence: host override, rtld special symbols, `main`, `sdk`, weak backstops. 49 symbols are defined in both modules and `main` wins
(operator new/delete, std typeinfo, `end`, `nn::os::Mutex::~Mutex`; list with `snap_boot.exe shared`).

## Boot sequence in `run`

Load both modules, `os_init` (with the musl thread-pointer variables), main thread adoption, `DT_INIT` of sdk then main (each `_init` walks its
own `init_array`; calling the array again would run every static initializer twice), `nninitStartup`, `nnMain`. Trace lines are timestamped:
first call of every host function, every SVC (first 3 per number), guest exceptions, and every initializer index. A stall watchdog prints the
main thread's guest frame after 8 s without output. Unresolved indirect targets append to `analysis/recomp/indirect_misses.tsv`.

**LOD and draw distance (2026-11-12, `host/mods/lodanim.cpp`, all off by default):** `SNAP_LOD_DIST=<1..8>` moves the LOD0/LOD1/LOD2 switch distances out by that factor (display-only since 2026-11-16: the cull no longer moves with it, and the LOD switches are off while photo select re-renders the photos for scoring), `SNAP_LOD_FORCE0=1` makes every drawn model LOD0, `SNAP_DRAW_DIST=<1..8>` moves the not-drawn (cull) distance out; the old `SNAP_DRAW_DISTANCE=<m>` still scales both together. `SNAP_FULL_RATE_ANIM=1` rebuilds far models' render job every frame (original: every 2nd frame at LOD1, every 4th at LOD2). F1 Graphics > View rows: Detail distance (`gfx_lod_dist`, Original, High or Max), Always full detail (`gfx_lod_force0`), Draw distance (`gfx_draw_dist`, `SNAP_DRAW_DIST`; an old saved `gfx_draw_distance` is migrated to `gfx_lod_dist` + `gfx_draw_dist` at load), Full-rate distant animation; all restart-required, in no quality preset. Test aids: `SNAP_DRAW_DISTANCE_AB=<P>` (the mod applies only on odd blocks of P presents; with the `lod_yard` route and `NVN_FRAME_DUMP_AT` at 16k+7/16k+15 the pair is the same camera pose), `SNAP_TRACE_LODSEL=1`.

## Render pass toggles (2026-11-12, `host/mods/gamemods.cpp`, hook `0x989048`)

- `SNAP_PASS_DUMP=1` (with `SNAP_GM_TRACE=1`) logs every pass once: `pass <ptr> name "<Name>" order=<n> on=<0|1> ... class protected|toggle|unverified`. A pass object keeps its name as a libc++ `std::string` at `+0x40` and its enable byte at `+0xf0`. 42 passes exist: DynamicShadow, StaticShadow, ZPrepass, Backmost, Opaque, OpaqueGround, HighResolutionTranslucent, SSAOCreate/BlurX/BlurY/Composite, DownsizeTarget, Effect, Sea, BlendBuffer, CompositeBlendBuffer, MipmapLevel0..5, BloomCreate, BloomDraw, Final, Evaluation, GaussianBlurX/Y, DOFBlurX/Y, DOF, ChromaticAberration, FilmGrain, Monotone, Capture3D, CaptureCompress, PhotoEditing, PhotoEditingFrame, Report, ResizeTexture, UI, Fade.
- `SNAP_PASS_OFF=BloomCreate,BloomDraw,DOFBlurX,DOFBlurY,DOF,FilmGrain,ChromaticAberration` (case-insensitive) zeroes the enable byte before the pass loop reads it, so the pass is never recorded. Only those seven names are accepted by default. Photo and evaluation passes (Evaluation, PhotoEditing, PhotoEditingFrame, Capture3D, CaptureCompress, Report, ResizeTexture, Final, UI, Fade, thumbnail names, DownsizeTarget, CompositeBlendBuffer) are always refused with a log line; any other name is refused as unverified unless `SNAP_PASS_FORCE=1` (test aid).
- `SNAP_PASS_AB=<P>`: the listed passes are off only in even blocks of P presents (P=1: alternate presents).
- F1 Graphics rows Bloom, Depth of field, Film grain, Chromatic aberration (`gfx_pass_bloom`, `gfx_pass_dof`, `gfx_pass_grain`, `gfx_pass_aberration`, each `default`/`off`, restart needed) are joined into `SNAP_PASS_OFF` by `runtime/hle/settings.cpp`.
- What the game does itself: the course view has Bloom on for the whole run; DOF, FilmGrain and ChromaticAberration are on only in a few scenes (the course has them off), so those three toggles change nothing in normal play.

## Performance switches (perf pass 4, 2026-11-13; `analysis\perf\results.md` "pass 4")

All on by default; each env var restores the old path.

- `SNAP_SYNC_FULL=1` (or `SNAP_WRITE_WATCH=0`): full-range copies of bound pool ranges into the Vulkan mirrors, as before. Default: guest reservations carry `MEM_WRITE_WATCH` (`runtime\os\os_memory.cpp`) and `SyncPoolRangeOnce` (`runtime\nvn\nvn_vk.cpp`) copies only the pages `GetWriteWatch` reports written since the last query. Pools that are not page aligned or not in a watched reservation, and pools with fragment-shader write-backs (photo evaluation), keep full copies. `SNAP_SYNC_FULL=1` also drops the reserve flag.
- `SNAP_MODS_CHUNK_CACHE=0`: build the replacement DRP chunk (copy + crc32 of the whole file) on every hit, as before. Default: built once per override and shared read-only (`runtime\mods\mods.cpp`).
- `SNAP_SCRATCH_POOL_MB=0`: exact-size scratch buffers freed on retire, as before. Default: retired scratch buffers are recycled by power-of-two size class (64 KB to 32 MB), at most 128 MB kept (`SNAP_SCRATCH_POOL_MB=<n>`).
- `SNAP_UPLOAD_PREFETCH_MB=0`: static textures are deswizzled on the replay thread at first sample, as before. Default: `TextureInitialize` queues the deswizzle on 1 to 4 below-normal workers; the upload checks the guest bytes are unchanged (hash before and after the job and at upload) and copies the result; anything else takes the old path. At most 1536 MB staged (`SNAP_UPLOAD_PREFETCH_MB=<n>`, 0 to 8192, oldest dropped first; pass 6 raised it from 384, which dropped every texture the scene-change bursts then missed). Not for ASTC, linear, multisample or textures over 48 MB.
- `SNAP_UPLOAD_PREFETCH_IMAGE=1` (pass 5, OFF by default): the prefetch worker also creates the VkImage and its memory after a successful deswizzle (`CreateTexImage`, `runtime\nvn\nvn_vk.cpp`). `EnsureImage` adopts them when the builder and host size are unchanged (`upload_pref_img` column); otherwise the worker's image is destroyed. Two A/B pairs: `ensureimg` per burst went from 41-49 ms to 15-29 ms, but hub-burst upload time doubled and scratch allocation slowed, so it stays off.
- `SNAP_UPLOAD_PREFETCH_HELP=1` (pass 5, OFF by default): when the replay reaches a texture whose prefetch job has not started, the replay thread runs up to 8 queued jobs from the front of the queue, then its own, while the workers keep going. Default, as before: the job is cancelled and the texture is deswizzled on the replay thread. No measured gain; burst misses are rarely "not started" (`pref_miss_*` columns: `none` never queued or dropped, `fail` bytes changed during the job, `hash` bytes changed since, `late` not started; pass 6 adds `pref_miss_drop` evicted by the budget, and per `TexPrefetch` call `pref_skip_kind`, `pref_skip_nosrc`, `pref_skip_budget`).
- `SNAP_TEXMEM_BLOCK_MB=0` / `SNAP_SCRATCH_BLOCK_MB=0` (pass 7): one `vkAllocateMemory` per texture image / per scratch buffer, as before. Default: allocations up to a quarter block come from shared 64 MB blocks (`runtime\nvn\nvn_submem.cpp`; first fit, coalescing free list; images need no dedicated allocation). `SNAP_SUBMEM_SPARE=<n>` (default 2, 0 to 8): a helper thread keeps n empty blocks of each pool ready, so a burst does not allocate blocks on the replay thread; 0 = allocate on demand. `SNAP_SUBMEM_RESERVE=<n>` (pass 9, default 8, 0 to 32): from the first image allocation at boot, the helper thread keeps the image pool at n blocks or more (empty target = max(spare, n - blocks in use)), so the first hub entry finds its blocks ready; 0 = spare blocks only.
- `SNAP_POOL_BACK_AHEAD=0` (pass 7): a memory pool's Vulkan mirror (allocation plus whole-pool copy) is created at the first draw that binds it, as before. Default: `BufferInitialize` queues it on a prefetch worker (needs `SNAP_UPLOAD_PREFETCH_MB` > 0; pools over 256 MB stay lazy). New column `backing_ms`: mirror creation time on any thread.
- `SNAP_UPLOAD_PREFETCH_STAGING=0` (pass 8): prefetch workers deswizzle into a heap buffer and the upload copies it into a new staging buffer on the replay thread, as before. Default: the worker writes straight into its own mapped staging buffer (a scratch-block range, `nvn_submem.cpp`), and the upload records the GPU copy from it with no CPU copy. Textures over 16 MB, or with `SNAP_SCRATCH_BLOCK_MB=0`, use the old path. Staged bytes still count against `SNAP_UPLOAD_PREFETCH_MB`, but they now sit in pinned host-visible memory.
- `SNAP_DUMP_EVERY=<s>` (pass 5, debug, `host\boot.cpp`): guest thread dump (wait kind, wait key, guest call chain per thread) into the trace every s seconds. For hangs that keep presenting, which the 8 s silence watchdog never sees.
- New `SNAP_PERF` columns: `ensureimg_ms`, `view_ms`, `rendbegin_ms` (replay sub-scopes), `upload_pref` (uploads served by the prefetch); `upload_bytes` is now filled; `poolsync_bytes` counts bytes actually copied.
- `SNAP_NVN_CMD_RECYCLE=0` (CPU sampler pass, `analysis\perf\cpu_profile.md`): every recording grows a fresh `vector<Cmd>`, as before. Default: released lists' vectors are pooled and a new recording reserves the largest recording seen.
- `SNAP_REPLAY_SCRATCH=0`: `PrepareTextures`, `WriteDescriptors`, `DoDraw` and `DoDispatch` allocate their per-draw vectors and deques, as before. Default: thread-local scratch vectors.
- `SNAP_POOLSYNC_SNAPSHOT=1` (OFF by default): one whole-pool `GetWriteWatch` per replay epoch lets clean ranges skip the per-range syscall; the gain was too small to keep.
- `SNAP_VIEW_LAST=0` (CPU round 2, 2026-11-14): `TextureImpl::View` builds the string key and probes the view map on every call, as before. Default: a repeat of the texture's last view returns its cached handle.
- `SNAP_VA_CACHE=0`: `Host::ResolveAddress` takes `vaMutex` and walks the pool map on every call, as before. Default: a per-thread 4-entry cache, invalidated by `Host::vaGen` when a pool is registered or unregistered.
- `SNAP_CS_ELIDE=0` (perf pass 18, 2026-11-15; `analysis\perf\results.md` "GPU cost at the owner's profile"): a full memory barrier after every skinning dispatch, as before. Default: no barrier between back-to-back dispatches of the skinning compute shader (key `ada6de87ec03`), about 25 per frame; the barrier is emitted before the first other command that follows, before any other dispatch, and before a dispatch whose buffers or destination range overlap an earlier one in the run (`DoDispatch`, `runtime\nvn\nvn_draw.cpp`). Saves about 0.35 ms of GPU time per frame at 4K. `SNAP_PERF` / `SNAP_PERF_SHARED` frames csv: last two columns `cs_elided` (barriers dropped) and `cs_forced` (dropped barriers the overlap guard put back; any value above 0 means the guard fired).
- `SNAP_GPU_PASSES=<csv>` (debug, off unless set; with `SNAP_PERF` or `SNAP_PERF_SHARED`): per-pass GPU timestamps, one row per segment; `python -I analysis\perf\gpu_passes.py <perf prefix> [--strip-ptr]` prints ms per frame per pass.
- `SNAP_PRESENT_AREA=0` (2026-11-15; `runtime\nvn\nvn_downsample.cpp`, shader `runtime\nvn\shaders\area.comp`, regenerate the header with `python -I runtime\nvn\shaders\make_area_spv.py`): the present step shrinks a window texture larger than the window with one bilinear `vkCmdBlitImage`, as before. Default: when the window texture is larger than the window on both axes, a compute pass box-filters it (each output pixel is the area-weighted mean of every source texel it covers) into an RGBA16F image of the window size, and that image is blitted 1:1 with NEAREST to the SDR swapchain or the HDR scratch. Same size stays NEAREST, upscales stay LINEAR, and frame generation's image keeps the bilinear blit (it is not sampleable). Bilinear on a non-integer step (5760 to 3840) reads only the 4 texels nearest each output pixel and skips the rest, so thin foliage edges shimmered. GPU pass "present area" in `SNAP_GPU_PASSES`: 0.13 ms median at 5760x3240 to 3787x2130 (Florio ride, medians of 2 runs on and 2 off); the present step went from 0.09 to 0.20 ms. `NVN_FRAME_DUMP_POST` dumps show the result; plain `NVN_FRAME_DUMP` dumps are the window texture before scaling. `SNAP_PRESENT_AREA_PAIR=<dir>` (test aid): on route `shot=` frames, also blit the same source with the old bilinear filter and write `pair_p<present>_area.bmp` and `_bilinear.bmp` (window size, 24-bit). Zoom strip: `docs\screenshots\compare\present-area-zoom.webp`.
- **No console window.** `SnapRecomp.exe` (the builder's link, `builder/snapbuild/pipeline.py`) is a Windows-subsystem program with `mainCRTStartup` as the entry point, so double-clicking it shows no console. `attach_parent_console()` at the top of `main` (`boot.cpp`) attaches to the parent terminal when the output is not redirected, so `--import-save` and the like still print there. A pipe or file given to the process is used as it is. `host/build_snap.py --gui-subsystem` links the dev exe the same way; the default stays a console program so `& snap_boot.exe` in a shell waits for the game.
- **Launcher window.** `host/launcher_ui.cpp` draws its own title bar (minimize, maximize, close, drag to move, edge resize; `WM_NCCALCSIZE` and `WM_NCHITTEST`), line icons on the home rows, and checks the game folder on a worker thread: the Play tile shows a moving bar and the file count (Checking), then Play, or Fix folder when the files do not match; the first chip shows the result and starts the check again when clicked. `SNAP_LAUNCHER_UI_CHECKING=1` forces the checking look for a test picture. "When the game closes" (Display page, key `return_to_launcher`, `SNAP_RETURN_TO_LAUNCHER=1`): after the launcher-started game ends or its window closes, `maybe_return_to_launcher()` (`boot.cpp`) starts the exe again with the original environment (not with `SNAP_LAUNCHER_AUTOPLAY`). The HD textures dialog (`hd_run_with_bar`, `launcher.cpp`) runs `hdtex` hidden in a job object with a progress bar read from its `--json` output; Cancel ends the job (exit 1223) and the finished textures are kept.
- `SNAP_AA_COPY=1` (perf pass 20; off unless set): keep the copy of the tonemap target into the AA scratch image when SMAA + CAS run (default: the edge and blend passes sample the target directly, same output). `SNAP_PRESENT_CLEAR=1` (off unless set): clear the swapchain black before the present blit even when the blit covers all of it. `SNAP_AA_VERIFY=1` (test aid): every 100th AA step runs both chains and logs `aa verify: ... identical|DIFFER`; leave it off in normal play.
- Measuring `SNAP_FPS=144` (perf pass 21): the saved window `frame_cap` (default 0, `%APPDATA%/SnapRecomp/window.json`) still applies as `effective = min(target, frame_cap)`, so a saved cap of 120 holds every 144 run at 8.33 ms. Set `SNAP_WINDOW_FRAME_CAP=0` for the run.
- `SNAP_POOLSYNC_SMALL=<bytes>` (perf pass 19, 2026-11-15; default 4096, `0` = the old path): a bound buffer range of this size or less is copied straight from guest memory into the mirror instead of a `GetWriteWatch` call per range (about 950 syscalls per 4K frame). Same bytes reach the GPU; +15% average fps and +20% 1% low on the uncapped 4K ride (`analysis\perf\results.md`). Diagnostics, off unless set: `SNAP_POOLSYNC_HIST=1` (call histogram on stderr), `SNAP_POOLSYNC_VERIFY=1` (compare the mirror with guest memory after each sync of a range up to 64 KB, prints `[poolverify]`; `=2` checks only every second range larger than `SNAP_POOLSYNC_SMALL`, cheap enough for a ride).
- `SNAP_SAMPLE=1` or `=<path prefix>` (profiling, off by default; `runtime\os\sampler.cpp`): host sampling profiler. Knobs: `SNAP_SAMPLE_US` (period, default 1000), `SNAP_SAMPLE_WRITE_S` (rewrite interval, default 20), `SNAP_SAMPLE_MIN_KCYC` (idle gate, default 100). Read with `python -I analysis\perf\cpu_profile.py <prof> --map <that build's snap_boot.map>`.

## Shiny Pokemon (2026-11-14, `runtime/nvn/nvn_shiny.cpp`, off by default, cosmetic only)

Build the pack once from your own extracted textures: `python -E -P scripts\hd\make_shiny_pack.py [--species 7,16] [--sheet DIR]` (system Python; output `%APPDATA%\SnapRecomp\mods\shiny-pokemon\`, outside git, about 217 MB; colors in `scripts\hd\shiny\shiny_colors.txt`, explicit overrides in `palettes.json`). Every body texture of a species is recolored, so multi-texture models match. Most species use an approximate hue rotation, not the official shiny colors; Chandelure, Vivillon, Sawsbuck, Florges, Minior, Frillish and Deerling have no shiny yet.

- `SNAP_SHINY=1`: enable (default 0). Menu row "Shiny Pokémon" (native Photos tab row 5, F1 Photos; 2026-11-14): Off (default, key `shiny` erased) / Rare 1/4096 / Common 1/512; `settings.cpp` turns the key into `SNAP_SHINY=1` + `SNAP_SHINY_ODDS=<n>` at start, so it applies after a restart (`nvn_shiny.cpp` reads the switches once). No forced/test choice in the menu.
- `SNAP_SHINY_ODDS=<n>`: 1 in n per spawned instance (default 4096; 1 = every instance of a pack species).
- `SNAP_SHINY_SEED=<n>`: fixed seed for repeatable rolls (default: random profile seed in the pack folder, a new roll each launch).
- Subject-accurate photo badge (rounds 4 and 5): a shutter thumbnail is flagged only when a shiny twin's draws cover at least 300 px (1080x720 equivalent) of the frame it copies, measured with one occlusion query per twin draw; the log line is `[shiny] thumbnail ... SHINY subject (<species> <px> px)` or `no shiny subject`. The badge is an iridescent seal with a sparkle trio, drawn by the overlay only (never into pixels, so saved JPEGs stay vanilla), on the ride pop-up, photo select, Score polaroids (strict per-texture lineage, no last-shown heuristic), album save detail and, after a restart, Your Info, the lab album grid, album detail and photo editing. Saved shiny photos are linked by JPEG CRC32 in `shiny_photos.jsonl` (in `SNAP_SAVE_DIR` when set, so tests never touch the owner's `%APPDATA%`; else the pack folder). A glint (soft white band, 1 s every 4 s) crosses the seal, off during capture and Score. Debug: `SNAP_SHINY_ONLY=<species substring>`, `SNAP_SHINY_MINPX=<n>`, `SNAP_SHINY_GLINT_HOLD=1`; mixed run: `SNAP_SHINY_ONLY=Pichu SNAP_SHINY_MINPX=5000` on route `shiny_badge`. Proofs: `analysis/shiny/final_*.png`, `negative_frame_no_badge.png`. Routes: `shiny_badge` (ride to album save; keep the save, copy it to `build/fixtures/saves/shiny_album`), `shiny_badge_album` (restart, Lab, Album), `shiny_badge_neg` (no Pokemon in the shot), `shiny_inert_edit` (deterministic inert check: with `SNAP_SHINY` unset the re-encoded JPEG is byte-identical, CRC32 ca1f8cc1).
- `SNAP_SHINY_WINDOW=<presents>`: debug only, presents between the flagged photo copy and the JPEG encode (default 60).
- Shiny photos: a photo shot while a shiny is on screen shows the sparkle seal badge (see above) and the capture shows a "Shiny <name>!" pill with sparkles and a chime. Research colors: `python -E -P scripts\hd\shiny\merge_research.py`, then rebuild the pack. For screenshots of the badge set `NVN_FRAME_DUMP_POST=1`.
- `SNAP_SHINY_FORCE=<hex key>`: that body texture is always shiny. `SNAP_SHINY_TRACE=<hex key|all>`: log draw state or all texture keys. `SNAP_SHINY_DIR=<dir>`: pack folder.

## HD photos (2026-11-14, `runtime/nvn/nvn_hdphoto.cpp`, off by default, display only; `analysis\core\hd_photos.md`)

- `SNAP_HD_PHOTOS=1` (settings key `hd_photos`; F1 > Photos > "HD photos", restart; native Settings > Photos tab row 2 once the native pack is rebuilt with `scripts/ui/make_native_options.py --out <mods root>`; route `host/routes/hd_native_opt.txt` checks it): photos taken under a scaled frame (4K DLAA, DLSS, FSR, render scale above 1) also get a 2160x1440 copy, shown in place of the game's image wherever a photo is drawn at least 480 UI units wide (photo select preview, score screen photo, album save, album full view). Photos drawn smaller (the strip of small photos in photo select, the shot preview during the ride) get a box-filtered level of the HD copy sized to the draw (1080x720, 540x360 or 270x180 by render-target pixels), only when that level is at least 1.2x the width of the game's own texture; `SNAP_HD_PHOTOS_SMALL=0` keeps the game's texture for those draws (no F1 row; `SNAP_HD_PHOTOS` is the switch). A level costs 0.2 to 3 MB, 48 MB LRU. Album grid cells show a mip level of the HD crop their thumbnail JPEG shows (thumbnail-to-photo index `photos_hd\thumbs.txt`, matched by content at encode; photos taken before 2026-11-14 keep the game's thumbnail; route `host/routes/hd_album_grid.txt`). Off (default): the hooks return at once and nothing is captured, written or swapped.
- Capture: every game capture pass (`CapturePS`/`CaptureLanczos3PS` into the 360x240 shutter thumbnail or the 1080x720 re-rendered photo) also triggers a `vkCmdBlitImage` of the scene region that draw samples (read from its texture coordinates) into a 2160x1440 sRGB image, read back and encoded on a BelowNormal worker as a q95 JPEG (WIC), about 1.2-1.5 MB per photo. Skipped when the region is under about 1411 host pixels tall (no gain over 1080x720) or the capture has a brightness offset.
- Storage: `%APPDATA%\SnapRecomp\<profile save root>\photos_hd\<crc32>.jpg`, keyed by the CRC32 of the game's own JPEG (written atomically: temp file, flush, `MoveFileEx` replace). A game JPEG is tied to its capture by content (135x90 box averages, mean difference under 10); no match means no HD copy. Saves are encrypted, so deleted photos cannot be detected: the folder keeps at most 1200 files (oldest removed) and stale temp files are deleted at start.
- Display: the album's photo texture is recognized when the game decodes that JPEG (CRC) and uploads the pixels; the HD file is read and decoded on the worker when the photo is first drawn large, LRU GPU budget 256 MB (about 20 photos). Missing or corrupt files fall back silently to the game's image. Photo Editing screens keep the game's image (their effect passes feed the edited photo the game saves). Profile duplicate and folder import copy `photos_hd` along (`runtime/hle/profiles.cpp`). Nothing the game reads (save, JPEG, score inputs) changes.
- `SNAP_HD_PHOTOS_LOG=1`: `[hdphoto]` log lines (captures, matches, files, swaps).
- `SNAP_HIRES_PHOTOS=1` (2026-11-15; settings key `hires_photos`, F1 > Photos > "Full-size photo PNGs", restart; off by default, needs HD photos): each 1080x720 re-render (the score screen and the album save; the album JPEG comes from it) also blits its scene region (LINEAR) through an sRGB image into a host buffer of its own, at the region's full height in the photo's 3:2 shape (5760x3240 becomes 4860x3240 at Enhanced 4K; the game squeezes its 16:9 re-render into the 3:2 photo, so a 1:1 copy looks stretched). After the capture's readback, the screenshot writer thread (`nvn_shot.cpp`) writes it to `photos_hd\pending\capture<id>.png`; when the album JPEG's CRC ties to the capture it is renamed to `photos_hd\<crc32>.png` beside the HD JPEG. The 360x240 shutter captures get none (72 per ride would be about 1.5 GB of PNGs). At most 3 copies in flight (about 63 MB each). Unclaimed pending PNGs are deleted at the next session's first write; the 1200-file trim removes a PNG with its JPEG. The game's photo, JPEG and score inputs are only read.

## Screenshot key (2026-11-15, `runtime/nvn/nvn_shot.cpp`)

- F12, or View + R3 on a controller (physical buttons, never remapped; the game still sees both), saves a lossless 24-bit PNG of the frame at the render size, before the present downscale: 5760x3240 with Enhanced at 4K. By default the HUD is left out: the copy is taken right after the tonemap / AA draw (the point frame generation takes its HUD-less copy at), on the frame after the press, and the screen keeps its HUD. A frame with no tonemap draw for 20 presents (menus, loading) is saved as shown, and so is a frame whose HUD-less copy is blank (photo select runs the tonemap draw over an empty scene; the frame as shown is copied with it, about 75 MB more for that one shot).
- Folder: `SNAP_SHOT_DIR`, else settings key `shot_dir` (F1 > Photos > "Screenshot folder", live), else `Pictures\Snap Recomp`. Files are `SnapRecomp_<date>_<time>_<n>.png`. `SNAP_SHOT_HUD=1` or `shot_hud` ("Include the HUD", live) saves the window texture as shown instead.
- The GPU copy is recorded on the present path; a BelowNormal writer thread waits for it and encodes the PNG (WIC, a temp file then renamed), so the present thread does not wait. A 5760x3240 PNG takes a few seconds to write; up to 3 shots can wait for the writer (about 75 MB each), and a press beyond that is ignored (logged).
- Route token `screenshot` presses the key once when its line starts (tests: set `SNAP_SHOT_DIR` to a scratch folder). Log lines start with `[shot]`.

## Free camera (2026-11-15, `runtime/hle/freecam.cpp`, `host/mods/freecam.cpp`, `runtime/nvn/nvn_freecam.cpp`; `docs/freecam.md`)

- F6, or L3 + R3, during a ride: the NEO-ONE waits on its rail and the camera flies (WASD and mouse, or both sticks; speed steps, roll lock, FOV on Z/X or ZL/ZR). Same key again: the rail camera comes back exactly and the ride goes on. F7 / X freezes the world, F9 / Y saves a HUD-free PNG at render size to `<screenshots>\freecam\`. While it is on the game sees an idle pad (no scored photo off the rail). Options: `SNAP_FREECAM=0` (hotkeys off), `SNAP_FREECAM_FREEZE=1`, `SNAP_FREECAM_LOG=1`.
- Route tokens: `fc=on|off|toggle`, `fcmove=`, `fclook=`, `fcpose=`, `fcfov=`, `fcshot`, `fcfreeze=on|off`, `fcspeed=`. Their state stays after the line ends: say `fc=off` and `fcfreeze=off` yourself. Routes `freecam_florio`, `freecam_score` (free camera and a frozen world mid-ride, then Score), `freecam_beach`; `SNAP_ROUTE_LOG=1` logs the film counter.

## Mod hooks (2026-11-16, `runtime/hle/modhooks.{h,cpp}`, `host/mods/modhooks.cpp`; `docs/mod-hooks.md`)

Named before / after hooks on game functions, no regeneration of `main`: the function's dispatch-table cell gets a trampoline. Off unless enabled: `<mods root>\hooks.json` (`{"hooks": {"<mod>/<point>": true}}`) or `SNAP_MOD_HOOKS=<mod>/<point>,...` (`*` = all registered, `-` in front = off). A point nobody enabled is never installed, so the cost with no hook is zero per call. Points: `Camera.RideBuild`, `Ui.Event`, `Ui.OptionEvent`, `Ui.SetState` (arguments in `docs/mod-hooks.md`); each has no direct caller in `main`, because a direct lifted call skips the table, and the start-up log says how many direct callers a point would miss. A hook that faults or throws is switched off for the run and the guest registers are put back. Samples (off): `sample-ui-log/Ui.SetState`, `sample-ride-camera/Camera.RideBuild`, `sample-fault/Ui.Event` (shows the guard). Test: `modhooks` in `tests\hle\run_tests.ps1`.

## Ultrawide (2026-11-15, `runtime/nvn/nvn_wide.cpp`, on by default for windows wider than 16:9; `docs/ultrawide.md`)

- `SNAP_ULTRAWIDE=auto|off` (default auto; settings key `gfx_ultrawide`; F1 > Graphics and the launcher, applies live). Auto widens the 3D view by k = window aspect / (16/9), capped at 4, with the same vertical field of view; the HUD, menus, videos and photos stay 16:9 in the middle. Off, or a window at 16:9 or narrower, leaves every path as before. Log lines: `ultrawide: auto, window client WxH, factor k`, and at the first photo Evaluation `ultrawide: photo Evaluation uses the 16:9 projection`.
- Developer switches: `SNAP_WIDE_LOG=1|2|3` (decisions; `SNAP_WIDE_LOG_FROM=<present>`), `SNAP_WIDE_TRACE=1` (camera callers), `SNAP_WIDE_SITES=0xOFF:reg,...` (extra camera sites), `SNAP_WIDE_NO_C3=1` (16:9 effect cameras stay narrow).

## Scheduler switches (nn::os review fixes, 2026-11-13; `analysis\core\review_os.md`, `runtime\os\os_kernel.cpp`)

All fixes are on by default. `SNAP_SCHED_LEGACY=1` (or `all`) restores every old path below; a comma list of names restores only those (for example `SNAP_SCHED_LEGACY=migrate,yield`). The run log prints `SNAP_SCHED_LEGACY: old scheduler paths 0x..` when any is set.

- `migrate` (finding 1): no core migration. Default: a core waiter that cannot use a free core asks a holder that can use it to move there, the way Horizon's idle core pulls a thread. The holder moves at its next poll point without giving up its run; if it does not poll within `SNAP_OS_OVERCOMMIT_US` (2 ms), the waiter gets an overflow core. This is the loader (p31, pinned to core 2) behind a JobWorker on core 2 while core 0 is idle.
- `yield` (finding 2): yields only to equal or better priority. Default: `svcSleepThread(-2)` and a spin gate that has spun 4,096 passes in a row hand the core to its best waiter whatever its priority (YieldToAnyThread); the yielder then does not preempt that thread for `SNAP_OS_YIELD_ANY_US` (1000).
- `unboost` (finding 3): a core holder whose priority gets worse (end of a priority-inheritance boost, ChangeThreadPriority) notifies nobody, so better core waiters wait up to the 20 ms safety-net recheck. Default: they are woken and the holder is asked to yield.
- `deadline` (finding 10): the 2 ms overflow deadline is armed once per core wait. Default: it restarts when the holder asked to yield changes, and is cleared when nobody is.
- `hostblock` (finding 9): file reads (`runtime\hle\fs_core.cpp` `Fs::read_file`) and VAMM retry sleeps keep the virtual core. Default: they release it like a Horizon IPC wait (`os_io_begin`/`os_io_end`).
- `events` (finding 11): manual-clear TimerEvent/LightEvent waiters re-check only the flag, so Signal followed at once by Clear can leave them asleep. Default: a waiter woken by a signal returns signalled.
- `evspin`: Event and SystemEvent `Wait`/`TimedWait`/`TryWait` calls that return without blocking are not check-ins. Default: they poll for preemption; a wait on a manual-clear Event that stays signalled, and a TryWait that fails or leaves a manual-clear Event signalled, count as a spin pass (`os_spin_gate`). The `LevelManager_LoadResource` loop at main+0x47d3e0 spins this way for minutes and used to hold its virtual core, so pinned threads ran only on 2 ms overflow grants.

Diagnostics: `SNAP_OS_STARVE_MS` (50) and `SNAP_OS_STARVE_LOG=1`. A core wait longer than that while another core is free counts in `OsStats::core_starved` (shown in the watchdog dump line `dump: ... starved N, migrations M/R, yield_any Y`), and with the log switch prints `[os] starved: thread ... holders: ...`. It should stay near 0; a pinned waiter behind a better pinned holder also counts. Each core waiter in the dump now shows how long it has waited (`id/prio/mask/ms`).
Not switchable (plain bug fixes): MultiWait signal fence, guest ticks read inside the pause lock, `InternalConditionVariable::TimedWait` far targets, thread start/destroy and join/destroy use-after-free, Link during WaitAny wakes the waiter, watchdog dump takes the scheduler lock (retries up to 500 ms; if still held it prints only the counters and skips the thread walk), `SNAP_OS_CORES=0` means unlimited.

## Load-time switches (load pass, 2026-11-13; `analysis\perf\results.md` "Load times")

On by default unless noted; each env var restores the old path.

- `SNAP_FNPROF_KEEP=1`: keep the per-function `FN_ENTER` hook on for the whole run, as before. Default: `run_initializers` (`host\loader.cpp`) turns it off after the init log unless a tracer (`SNAP_FNTRACE`, `SNAP_TRACE_LOD`, `SNAP_SKIPS_REC`, `SNAP_NEO_CAMFN`) had it on. It cost a hash lookup and three observer calls on every lifted function entry, about a fifth of the guest CPU samples in a boot profile.
- `SNAP_EVPROBE=1` (dev only, `host\mods\evprobe.cpp`): log the motion-event name of every photo record the film evaluator checks at course end (`evprobe ... tag=EV_Mot_Mis_...`); a request counts when a submitted photo carries its `MissionData` event name. Off: nothing is registered.
- `SNAP_AIM=1` (dev only, `host\mods\aim.cpp`, needs `SNAP_FNPROF_KEEP=1`): the route lines `aim <species|nearest|pose:<text>|off>`, `wait_pose <text>` and `shoot` point the ride camera at a live actor and photograph it (syntax in `runtime\hle\input_core.cpp`; example `host\routes\aim_florio.txt`). `SNAP_AIM_LOG=1` adds a census of the actors, their pose events, screen offsets and distances. More route parts: `near=<x>,<z>` on the `aim` line picks the actor closest to that world point, and the conditions `pose=<text>` / `act=<text>` (motion event / photo event of the target) gate `wait` and `repeat ... if=` lines; example `host\routes\aim_request_pichu.txt`. The target `act:<text>` (for example `aim act:Mis_ cap=5`) follows whichever actor's photo event contains the text and moves on after `cap` photos of one event, so one ride photographs every request event that comes up; `host\routes\req_sweep_*.txt` are such rides for every course. `ev:<alt>|<alt>...` is the same with alternatives, each `<text>` or `<name part>@<text>` (`CH_099@Sleep_01`; empty text = any photo of that species). `range=<m>` keeps a sweep from photographing from farther away, and `cap=<n>` on a plain target limits an `act=` condition to n photos of one event. With `SNAP_AIM_LOG=1` the tool also logs every call of the game's request check (main+0x500ba0) as `aim: reqcheck course= species= skip= commit= event=` plus the open requests for that species: `skip` is the photo record's status and must be 0 for a request to count. `SNAP_EVPROBE=3` logs every compare of two whole event names with its guest callers. `SNAP_AIM_EVTABLE=<species|1>` dumps a species' event table (motion event to photo event; `mis` = every species once, request rows only) and `SNAP_AIM_SHOTDUMP=1` dumps the photo records at each `shoot`. Request routes that completed: `req_sylveon_sleep` (needs `range=12`: the tool locks through scenery and a far photo gets status 5) and `req_glaceon_cliff` (a far target needs `zoom=2 tol=0.05`, because the photo's subject is the Pokemon nearest the frame center). A photo press must be held 6 frames. Never set in player builds or shipped settings.
- `SNAP_PACE_RELEASE_CORE=0`: the presenting JobWorker keeps its virtual core while the frame limiter sleeps, as before. Default: `Pace` (`runtime\nvn\nvn_vk.cpp`) runs inside `os_host_block_begin/end`, like vsync on the Switch, so pinned loader threads (`ResourceUncompress` p31) run during the sleep. `SNAP_NVN_RELEASE_CORE=1` (pass 3, still opt-in) releases the core for the whole submit/present/wait; it added nothing measurable on top.
- `SNAP_SHADER_FASTIO=0`: shader cache files (`.spv`, `.json`, `.efz`) read with `std::ifstream` byte iterators and one BCrypt provider open per hash, as before. Default: one sized Win32 read and one shared provider (`runtime\nvn\nvn_shader.cpp`). Boot creates about 6,200 programs.
- `SNAP_CRC_SLICE=0` (perf pass 10): the lifted CRC32 instructions run the byte-wise table loop, as before. Default: 8- and 4-byte `crc32x`/`crc32w` use slice-by-8 tables (`runtime\simd.h`, filled on first use, about 4.5x faster per step). The guest DRP checksum `main+0x15fe90` was about half the `ResourceUncompress` CPU at course start; cave start window 2.8 s to 1.7 s of that CPU. Takes effect only in a main library built after the change (`recomp\snap_build.py`; only `main_0015.c` uses CRC32).
- `SNAP_DRP_FASTDEC=0` (perf pass 11): the DRP chunk decrypt runs the guest loop, one call of the generator `0x5ec660` per 32-bit word. Default: the inline hook `rt_drp_decrypt` (`host\mods\drpfast.cpp`, `MAIN_INLINE_HOOKS` at `main+0x5ec9f0`) does the whole loop on the host and leaves the same data and generator state (about 20x faster; exact on 20,000 random chunks against the generated guest function, and on 16,000 real chunks in game). `SNAP_DRP_FASTDEC=check` runs both and logs `[drpfast]` mismatches. Needs main regenerated after 2026-11-14.
- `SNAP_OS_FREE_THREADS=<name>[,<name>...]` (perf pass 11, OFF by default): guest threads whose name starts with one of the names run without one of the 3 virtual cores (`runtime\os\os_kernel.cpp`). Tried with `ResourceUncompress,ResourceDecrypt`: their core waits went to 0, but slow start frames did not drop and in-course frames over 33 ms rose slightly (4 runs). Not recommended.
- `SNAP_OS_SPARE_CORES=<n>` (OFF by default): n extra virtual cores any thread may take whatever its mask; guests still see cores 0..2 (`GetCurrentCoreNumber`, core masks). One run: boot to title 20 s, but the lab hub then ran at about 9 fps with JobWorkers blocked on locks (the job system at `0x54c2ec` reads the core number, and two threads now "share" a core). Do not use for play.
- Not switchable: `NVN_DRAW_SIG`, `NVN_RT_DUMP` and `NVN_TEX_DUMP` are read once instead of by `getenv` per draw (`runtime\nvn\nvn_draw.cpp`; `getenv` was 4% of the exe samples).
- Tools: `SNAP_HLE_TRACE=open` logs only fs opens (load-phase markers: `title.drp` = title screen, `LC_101_A.drp` = lab, `LC_001_A.drp` = Florio). `SNAP_HOST_PROF=<file>` (`runtime\os\hostprof.cpp`, debug) samples the RIP of every thread that ran in the last 2 ms between `SNAP_HOST_PROF_FROM` and `_TO` seconds and writes `<exe offset> <count>` lines; map them with `snap_boot.map` (most samples land in ntdll waits; compare exe functions with each other).

## Frame pacing and vsync (2026-11-13/14; `analysis\perf\pacing.md`, `runtime\nvn\nvn_pace.{h,cpp}`)

- `SNAP_PACE=legacy`: the old frame limiter (steady 60 Hz schedule, 1.5 ms spin), the old free-running 16667 us guest vsync thread and the queue lock held across the acquire. Default: one frame clock for the limiter and the guest `nn::vi` vsync, locked to the monitor's vblanks when its refresh is a whole multiple of 60 (60, 59.94, 120 Hz), a steady-clock grid otherwise (144 Hz).
- `SNAP_PACE_LOG=<file.csv>`: one row per present (release interval, guest work, limiter wait, present wait, acquire, `vkQueuePresentKHR`, vsync ticks and phase, mode). Test overrides: `SNAP_PACE_MONITOR=<n>` (follow `\\.\DISPLAY<n>`), `SNAP_PACE_REFRESH=<hz>` (a value below the monitor's real refresh gets flagged within 3 vblanks and paced on the steady clock at the target rate), `SNAP_PACE_WAIT=0` (no `VK_KHR_present_wait` queue cap), `SNAP_PACE_VBLANK=0` (no vblank thread), `SNAP_VSYNC_HZ=<hz>` (legacy vsync loop period).
- `SNAP_VSYNC=on|adaptive|fast|off` (default `on`): swapchain present mode only. `on` = FIFO (no tearing; today's behavior). `adaptive` = FIFO_RELAXED (tears only when a frame misses its vblank). `fast` = MAILBOX (no tearing, lowest latency; the GPU may render frames that are never shown). `off` = IMMEDIATE (tearing; when locked to the display the frame is released 0.5 ms after the vblank, so the tear line sits near the top). A mode the driver does not offer falls back (`off` to MAILBOX, then FIFO; the others to FIFO); the choice is logged once as `[vsync] SNAP_VSYNC=<v> -> present mode <M>`. Every mode keeps the frame limiter (60 fps or the frame cap) and the guest's 60 Hz vsync, so ride and game timing do not change; with a VRR (G-Sync/FreeSync) display use `on` or `adaptive`. Aliases: `relaxed`, `mailbox`, `immediate`/`0`. Live switch: `extern "C" void snap_vsync_set(const char* mode)` (same values) recreates the swapchain at the next present, no restart; `snap_vsync_get()` returns the active value. Menu: the Vsync row (native Output tab, F1 Graphics) calls it and stores settings key `vsync` (2026-11-14). A game asking for present interval 0 still gets IMMEDIATE/MAILBOX as before.

## F1 menu look (2026-11-15, `runtime/ui/ui_skin.cpp`)

- The F1 menu is drawn in the game's Settings style (see `analysis/ui/f1_skin.md`). Scale follows the window height (`max(0.75, height / 1080)` times the Menu size row). The last page used is kept in settings key `menu_tab`.
- Pad: Back+Start opens, L / R switch pages, d-pad / stick move the cursor, left / right change a value, A opens a drop-down, B closes it, then the menu. Mouse and keyboard work as before.
- Test: `SNAP_UI_TEST=tests/hle/ui_f1_pad_script.txt` (controller only) and `tests/hle/ui_f1_tabs_script.txt` (every page, for captures).
- Languages: the menu follows the Game language row (`runtime/ui/ui_i18n_table.cpp`, French, German, Spanish, Italian, Japanese, Korean, both Chinese scripts). `python -I tests/hle/ui_i18n_check.py` checks the table; `tests/hle/ui_f1_lang_script.txt` shows every page in Japanese and German.

## Performance overlay (2026-11-14, `runtime/ui/ui_fps.cpp`)

- Settings key `fps_overlay` (env `SNAP_FPS_OVERLAY`): unset = Off (default, the original game), `fps`, `detailed`. Key
  `fps_overlay_corner` (env `SNAP_FPS_OVERLAY_CORNER`): `tl`, `tr` (default), `bl`, `br`. Both are live (F1 Graphics >
  Frame rate, native tab Performance, the 12th PC tab); the env var wins at start as for every option.
- F3 cycles Off -> FPS -> Detailed -> Off and stores the choice, unless F3 is bound to a game button in the controls map (then
  F3 stays the game's). F1 and F11 are untouched.
- FPS: one number, the average over the last 0.5 s, refreshed twice a second (green at 55+, yellow at 28+, red below).
  Detailed adds frame time (ms), 1% low (the average rate of the slowest 1% of frames in the last 10 s), CPU and GPU ms, render
  size (1920x1080 times the render scale) and output size, the upscaler / AA / CAS line, frame cap and vsync, and a frame-time
  graph of the last 2.5 s with a blue line at the target frame time (the frame cap or the game's pace rate; red segments are
  over 1.5x the target). CPU and GPU come from the `SNAP_PERF` timing source (`runtime/nvn/nvn_perf.cpp`): GPU = busy time of
  the timestamp pairs around every command buffer (collected one present late); CPU = the presenting thread's CPU time per
  frame (its cycle count over the TSC rate). Detailed switches those timestamps on at run time (`snap_perf_overlay_timing`);
  no CSV is written unless `SNAP_PERF` is set.
- Size follows the output height (1x at 1080p, 2x at 4K) on a semi-transparent dark box with a text shadow.
- Drawn by the host overlay (`ui_overlay.cpp`, the same path as the shiny badges and the F1 menu) into the swapchain image after
  the game's frame is composed, so it is never in the game's photos, album or any guest capture. Choice for dumps:
  `NVN_FRAME_DUMP_POST=1` dumps (the finished swapchain image) show it, as the player sees it; plain `NVN_FRAME_DUMP` dumps (the
  game's image before the host overlay) do not.
- Cost: Off = one relaxed atomic load per present. While on, the host overlay frame's CPU time is logged every 1200 presents as
  `ui: performance overlay <mode>: host overlay CPU per present: median .., p99 .., mean ..`. Measured 2026-11-14 on Florio
  (`core_florio_a`, 1280x720 hidden window, shared PC): FPS median 76-87 us, Detailed 92-112 us per present on the present
  thread; p99 4-11 ms spikes in both modes (most likely the BelowNormal present thread being preempted on the busy PC, not
  confirmed). Frame rate stayed at the 60 fps pace (Off 59.86, Detailed 59.79 fps over 140 s with `SNAP_PERF_SHARED`); GPU busy
  was 2.53 ms Off and 2.30 ms Detailed, so the overlay's GPU cost was too small to measure.

## High frame rate spike (2026-11-14, hifps worker; `host/mods/highfps.cpp`, `docs/high-fps-plan.md`, `analysis/perf/high-fps.md`)

Everything is off by default (default game = 60 fps, unchanged). `SNAP_FPS=90|120|144|165|240` calls `nvnHostSetFramePacing(hz)` at present 1 and after the `SNAP_FAST_BOOT` window (60 or unset: inert; read once at start). `SNAP_FPS_UNCAPPED=1` turns the limiter off for measurement. Diagnostics: `SNAP_FPS_TRACE=<csv>` (one row per clock update: caller, mode, dt, the three ms channels; forces `g_fnprof`, slow), `SNAP_FPS_MSWATCH=1` (hardware watchpoints on the clock's ms channels, `[highfps-w]` rip table, resolve in the link map). The 14 ms sleep floor in the main loop is never reached in play, so there is no sleep patch. Measured: real 120 is reachable (117.5 fps), 144 is not yet (115.7 fps, quiet PC, 1440p; 99 at 4K); ride length in seconds is unchanged.

Round 2 switches: `SNAP_ACQ_FENCE=0` (default on) restores the old swapchain acquire (GPU-side semaphore wait only); the new path waits an acquire fence on the CPU so guest `nvnSyncWait` no longer sits behind the blit (fixes ~16 ms waits at high fps). `SNAP_SYNC_TRACE=<ms>` logs `[synctrace]` for guest sync waits of that length or more. `SNAP_UPSCALE_HOLD_SCALE=0` restores the present-count shutter hold (default scales the hold by measured fps / 60).

## Ray tracing track (2026-11-14, rt worker; `runtime\nvn\nvn_rt.{h,cpp}`, `docs\lighting-rt-plan.md` section 4)

Everything is off unless `SNAP_RT=1`: with it unset the device, shader modules, pipelines and buffers are what they were. The device needs `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, `VK_EXT_transform_feedback` and the Vulkan 1.2 `bufferDeviceAddress` feature; if any is missing the log says `[rt] off: ...` and the game runs as before. Memory pools the game creates are not touched (only the layer's own buffers carry device-address flags).

- `SNAP_RT=1`: master switch. `SNAP_RT_CAPTURE=0`: device features only (M0), no vertex patch, no capture.
- `SNAP_RT_DEBUG=1|2|3`: ray-query overlay after the tonemap draw. 1 = sanity colors (green: ray hit matches the scene depth, red: hit in front of the depth, blue: hit behind it, magenta: depth but no hit, orange: hit on a sky pixel), 2 = hit-distance heat map, 3 = statistics only. `SNAP_RT_FLIPY=0|1` overrides the clip-y to screen-y mapping (default follows the window origin mode).
- `SNAP_RT_LOG=<n>`: stats every n presents (default 120 with `SNAP_RT_DEBUG`, off otherwise): triangles and draws captured, BLAS builds, live cache entries and memory, CPU record cost, GPU ms of convert+BLAS+TLAS (timestamps) and of the overlay, and the overlay's per-pixel class percentages.
- `SNAP_RT_ARENA_MB` (384), `SNAP_RT_BLAS_MB` (512), `SNAP_RT_SCRATCH_MB` (256): capture arena, BLAS storage and build scratch. A draw that does not fit is skipped and counted.
- `SNAP_RT_TTL=<frames>` (900): how long a draw seen in 3 or more frames stays in the TLAS after it last drew; draws seen fewer than 3 times drop after 2 frames.
- `SNAP_RT_REUSE=0|1|2` (1) and `SNAP_RT_STATIC_FRAMES=<n>` (4): a draw whose converted world-space vertices stayed within a tolerance (0.02 m + 0.2% of the distance from the eye) of the copy taken at its last BLAS build for n frames keeps that BLAS; the motion test runs in the convert pass and is read back 4 frames later, so a draw that starts moving is rebuilt up to about 4 frames late. A draw that was off screen in between is always rebuilt. 0 rebuilds every visible draw every frame; 2 alternates on and off per log window (for A/B timing under GPU contention; the log line says `BLAS reuse ON` or `off`).
- `SNAP_RT_REFLECT=1`: ray traced water reflections (implies `SNAP_RT=1` and the scene capture; off by default; also a live dev parameter). A reflection ray the screen-space march misses is traced into the captured TLAS; off-screen hits use a per-instance mean colour cache. `SNAP_WATER_DEBUG=5` colours the cases. Falls back to screen-space alone on a failure (`SNAP_RT_FORCE_FAIL=reflect` tests it). `docs/lighting-rt-plan.md` M5.
- `SNAP_RT_GI=1`, with `SNAP_SSGI=1` and `SNAP_LIGHTING=enhanced`: ray traced gather for the bounce-light chain (off by default; live parameter). `SNAP_RT_GI_RAYS` (1) and `SNAP_RT_GI_RADIUS` (6 world units) tune it; `SNAP_RT_FORCE_FAIL=gi` tests the fallback to the screen-space gather. `docs/lighting-rt-plan.md` M6.
- `SNAP_RT_AO=1`, with `SNAP_LIGHTING=enhanced`: ray traced ambient occlusion in place of GTAO in the lighting chain (off by default; live parameter). `SNAP_RT_AO_RAYS` (2) and `SNAP_RT_AO_RADIUS` (1.5 world units) tune it; `SNAP_RT_FORCE_FAIL=ao` tests the fallback to GTAO. Measured +1.6 ms at 1440p and +0.8 ms at 4K over GTAO with no visible gain, so it has no F1 row. `docs/lighting-rt-plan.md` M7.
- `SNAP_RT_KEYCB=0|1` (1): the draw identity for the BLAS cache includes a hash of the per-object vertex constants (tells apart draws that share one mesh); 0 keys by mesh identity only.
- Photo and Evaluation frames get no capture and no build (`FxIsPhotoFrame`, `FxWritable`); scene depth is only sampled, never written. Validation: `NVN_VK_VALIDATION=1`.
