# Command line

Every option the launcher offers for starting the game also has a flag, so another launcher, a shortcut or a script can start Snap Recomp without our window. `SnapRecomp.exe --help` prints the same list.

```
SnapRecomp.exe                      open the launcher
SnapRecomp.exe --play [options]     start the game without the launcher window
```

## Options

| Flag | What it does |
| --- | --- |
| `--play` | Skips the launcher window. The game-folder check still runs first. The process ends when the game closes, so the caller can wait on it. No window of ours opens: a start that cannot go on prints the reason and ends with exit code 1 (the game folder failed its check) or 2 (the converted files are missing or damaged). The first-start upscaler offer waits for a start without `--play`, and after a start that failed the safe-mode question becomes a printed line; add `--safe-mode` to use it. |
| `--safe-mode` | One start with mods, HD packs, enhanced effects and the shader cache off ([safe-mode.md](safe-mode.md)). |
| `--fullscreen`, `--borderless` | Borderless full-screen window. Both names do the same thing. |
| `--windowed` | Normal window. |
| `--steam-deck` | The Steam Deck preset: borderless 1280 x 800, a lighter 3D picture, a 150% HUD and Xbox button icons ([steam-deck.md](steam-deck.md)). Flags after it win. |
| `--monitor <n>` | Monitor number. 0 is the first. |
| `--width <px>`, `--height <px>` | Window size. |
| `--profile <name>` | The profile to play, or the one `--import-save` and `--export-save` use. |
| `--set <key>=<value>` | One setting for this start only. Repeat it for more settings. The settings file is not changed. |
| `--data-dir <folder>` | Keeps saves, settings, mods, caches and logs in this folder. The folder is created when it is missing. |
| `--help`, `-h`, `/?` | Prints the flag list and exits with code 0. |
| `--version` | Prints the version and exits with code 0. |

Without `--play` the launcher window opens with the flags already applied, and they hold for the game it starts.

When two window flags disagree, the last one on the line wins. A flag wins over an environment variable, and an environment variable wins over the settings file.

A mistyped flag stops the start: the exe prints `SnapRecomp: unknown option --name (see --help)` and exits with code 2. A flag with a missing or bad value does the same.

## Examples

```
SnapRecomp.exe --play --fullscreen --monitor 1
SnapRecomp.exe --play --steam-deck
SnapRecomp.exe --play --windowed --width 1280 --height 720 --profile Ash
SnapRecomp.exe --play --set fps_target=120 --set fps_overlay=fps
SnapRecomp.exe --play --data-dir D:\SnapData
```

## Keys for `--set`

`--set` takes the same keys the settings file (`settings.cfg`) uses, with the same values. A name that starts with `SNAP_` sets that environment variable for the start instead, for the options that exist only as a variable.

| Group | Keys |
| --- | --- |
| Frame rate | `fps_target`, `vsync`, `frame_generation`, `gfx_full_rate_anim`, `fps_overlay`, `fps_overlay_corner` |
| Resolution and upscaling | `gfx_render_scale`, `gfx_upscaler`, `gfx_upscale_quality`, `gfx_aa`, `gfx_smaa_quality`, `gfx_sharpen`, `gfx_ultrawide` |
| Detail | `gfx_aniso`, `gfx_lod_bias`, `gfx_draw_dist`, `gfx_lod_dist`, `gfx_lod_force0`, `gfx_pom`, `foliage_aa`, `leaf_translucency`, `soft_particles` |
| Lighting and shadows | `gfx_lighting`, `gfx_bounce`, `gfx_shadow_res`, `gfx_shadow_dist`, `contact_shadows`, `ssao_quality`, `gfx_ssao_samples`, `gfx_ssao_hbao` |
| Ray tracing | `rt_reflect`, `rt_gi`, `rt_gi_rays` |
| HDR | `gfx_hdr`, `gfx_hdr_peak`, `gfx_hdr_paper`, `gfx_hdr_expand` |
| Water | `water_ssr`, `water_caustics`, `water_foam`, `water_ocean` |
| Atmosphere and lens | `atmos_shafts`, `atmos_fog`, `atmos_exposure`, `atmos_bloom`, `atmos_clouds`, `atmos_sky`, `gfx_bokeh_dof`, `gfx_motion_blur`, `film` |
| Interface | `hud_scale`, `gfx_ui_font`, `ui_smooth`, `game_language` |
| Photos | `photo_export`, `photo_dir`, `hd_photos` |
| Play | `fast_boot`, `skip_cutscenes`, `skip_course_intro`, `neo_speed`, `freecam`, `freecam_freeze`, `spawn_rand`, `spawn_seed`, `return_to_launcher` |

Other keys from `settings.cfg` work too. Safe mode still ignores the settings it always ignores, even when `--set` names one.

## Data folder

A release build keeps everything the game writes in `portable_data` next to the built exe: `Roaming\SnapRecomp` holds saves, settings, profiles and mods, and `Local\SnapRecomp` holds caches and logs. The `portable.txt` file next to the built exe switches this on. [install-folder.md](install-folder.md) shows the whole folder.

`--data-dir <folder>` uses that folder in place of `portable_data`, with the same two subfolders. Nothing is copied into it, so a new folder starts with a fresh save. Import one with `--import-save` and the same `--data-dir`.

## Launcher actions

Each of these does one launcher job without the launcher window. The game does not start. Use one at a time, without `--play`.

| Flag | What it does |
| --- | --- |
| `--update` | Installs the newest version. The update runs in its own window. Exit code 0 means it started, 1 means the builder is missing. |
| `--check-install` | Checks the game files and prints the result. Exit code 0 when everything is in order, 1 when something is wrong. |
| `--report` | Makes the problem report zip and prints its path. Nothing is uploaded. |
| `--restore-backup` | Prints the save backups, newest first. `--restore-backup latest` or `--restore-backup <number>` puts that one back; the current save is backed up first. Exit code 0 done, 1 not restored, 2 wrong argument. |
| `--upscalers` | Prints DLSS, FSR and XeSS, whether each is installed, and where its license is. `--upscalers <name> --accept-license <name>` downloads that one from the vendor. Without `--accept-license` nothing is downloaded: the license is yours to accept. |
| `--hdtex` | Prints the HD texture state. With options it runs the HD texture tool with them, for example `--hdtex --level hd --dry-run`; `builder\HD-TEXTURES.md` lists the options. The exit code is the tool's. |

None of these opens a window. `--restore-backup`, `--upscalers` and `--hdtex` take everything after them as their own arguments, so put `--data-dir` and the other player flags before them.

## Setup flags

The `SnapRecomp.exe` you download answers these itself. Every other flag goes to the built game.

| Flag | What it does |
| --- | --- |
| `--game <folder>` | The folder with your game files. Setup opens with it chosen. |
| `--build-dir <folder>` | Where the build goes and where the exe looks for it. The default is the exe's own folder. |
| `--yes` | Builds without questions: accepts Microsoft's C runtime license, uses the defaults and closes when done. Exit code 0 means built, 1 failed, 2 no `--game` given. |
| `--setup` | Opens setup even when the game is built. |
| `--repair` | Rebuilds from the game files chosen before. |
| `--uninstall` | Removes the build. With `--yes` it does not ask, and it keeps saves and build tools. |
| `--desktop-shortcut`, `--start-menu-shortcut` | With `--yes`: also make that shortcut. Neither is made by default. |

A launcher that manages the game itself can build and start it with two lines:

```
SnapRecomp.exe --game D:\Dumps\Snap --build-dir D:\Games\Snap --yes
SnapRecomp.exe --build-dir D:\Games\Snap --play --fullscreen
```

Before a build exists, `--help` and `--version` print the setup's own text.

## Other commands

These do one job and exit. None opens the launcher.

| Command | What it does |
| --- | --- |
| `--import-save <folder or zip>` | Copies a save into the profile. Exit code 0 for success, 1 for a failure, 2 when the game is running. |
| `--export-save <folder or zip>` | Writes the profile's save out, with the same exit codes. |
| `--benchmark` | Plays the benchmark ride and writes a report. `--benchmark-capped` and `--benchmark-uncapped` pick the frame-rate mode, and `--benchmark-view <file>` shows an earlier report. |
| `--prepare-shaders` | Builds the shader cache ahead of play. Takes `--force`, `--visible`, `--list`, `--stop` and `--courses <list>`. |

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | The game or the command ended normally |
| 1 | A command failed |
| 2 | A bad command line, a data folder that could not be created, or converted game files that are missing or damaged |
