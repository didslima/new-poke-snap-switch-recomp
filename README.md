<p align="center">
  <img src="docs/logo/snap-recomp-logo.svg" alt="Snap Recomp: native PC recompilation" width="600">
</p>

<h1 align="center">New Poke Snap Switch Recomp — Unofficial Linux Port</h1>

<p align="center">
  <b>Unofficial, independently maintained Linux port of <a href="https://github.com/finalpantasy/new-pokemon-snap-recomp">New Pokémon Snap Recomp</a> by <code>finalpantasy</code>.</b><br>
  A native static recompilation (recomp) of a 2021 Switch photography game, working with your own copy of New Pokémon Snap 2.0.1.<br>
  The game's AArch64 code is lifted to C, built with clang, and run on a hand-written Horizon OS and NVN-to-Vulkan runtime.
</p>

<p align="center">
  <img alt="Platform: Linux x86_64 & Windows x64" src="https://img.shields.io/badge/platform-Linux%20x86__64%20%7C%20Windows%20x64-0078D6">
  <img alt="Graphics: Vulkan" src="https://img.shields.io/badge/graphics-Vulkan-AC162C">
  <img alt="Game version: 2.0.1" src="https://img.shields.io/badge/game%20version-2.0.1-informational">
  <img alt="Status: early playable" src="https://img.shields.io/badge/status-early%20playable-yellow">
  <a href="https://github.com/finalpantasy/new-pokemon-snap-recomp"><img alt="Upstream repository" src="https://img.shields.io/badge/upstream-finalpantasy%2Fnew--pokemon--snap--recomp-2ea44f"></a>
  <img alt="License: GPL-3.0 with additional permission" src="https://img.shields.io/badge/license-GPL--3.0-blue">
</p>

> [!NOTE]
> **Unofficial Linux Port:** This fork is an unofficial Linux port of **New Pokémon Snap Recomp**, maintained independently here.
> The original project is created and maintained by **[`finalpantasy`](https://github.com/finalpantasy)** at **[https://github.com/finalpantasy/new-pokemon-snap-recomp](https://github.com/finalpantasy/new-pokemon-snap-recomp)**, and the original codebase is licensed under **GPL-3.0** (with the section 7 additional permission; see [LICENSE](LICENSE)).

<p align="center">
  <a href="#linux-appimage-v120">Linux AppImage</a> ·
  <a href="#status">Status</a> ·
  <a href="#features">Features</a> ·
  <a href="#screenshots">Screenshots</a> ·
  <a href="#build-it-yourself">Download</a> ·
  <a href="#building">Building</a> ·
  <a href="#running-and-settings">Running</a> ·
  <a href="#testing">Testing</a> ·
  <a href="#project-layout">Layout</a> ·
  <a href="#roadmap">Roadmap</a> ·
  <a href="#faq">FAQ</a> ·
  <a href="#license-and-credits">License</a>
</p>

<p align="center">
  <img src="docs/screenshots/title.webp" alt="Title screen of the game running in the PC build" width="720">
</p>

> [!IMPORTANT]
> This repository holds source code only: no game files, keys, extracted assets or generated game code. You must supply your own legally dumped copy of version 2.0.1 (version 262144).

## Status

Early playable. The game boots to the title screen and the lab hub, all 14 courses play to the end, and photos can be taken, scored, saved and reloaded. Checks so far are scripted runs on a hidden desktop with a null audio backend. No full human play-through has been done.

| Area | State | Notes |
| --- | --- | --- |
| Boot, title, lab hub, map | Works | |
| Florio Park ride, photo select, scoring | Works | Photos sort by subject, thumbnails show, and the professor scores them |
| Courses | Works | A scripted sweep rode all 14 courses and 28 entries (Day, Night, Evening and Illumina variants) to the course end and the photo select. 26 of 28 reached the score screen with a scored photo. Florio Illumina and Volcano Illumina were not scored because the blind route cannot photograph the Illumina Pokémon. No crash, hang or device loss |
| Progression | Partial | Points registration, save and reload, level 2 unlock (Florio Night) and Photodex persistence pass from a real save. Professor requests were not reached |
| Story | Partial | Events 1, 2, 27 and 33 start and play, with cutscenes, dialogue, credits and the post-credits talk. The save after event 2 is verified. The save after credits and events 8 and 35 are not confirmed |
| Secrets | Partial | Scan pulses, Level 3 spawns and Night spawns pass. Florio Side Path, Mew, targeted orb, melody and fluffruit reactions, and special-pose photos were not triggered |
| Saves | Works | Written atomically with generations and backups. Every crash stage tested left the old or the new save, never a torn one |
| Audio, video, text | Works | Title, hub, map, Florio ride and photo select have music, shutter and ambience with no clipping. Cutscenes are in-engine, so there are no video files. English text and score numbers read correctly |
| Input | Partial | Keyboard, mouse and controllers (XInput, plus SDL3 when built with it). Rumble works on the host side with scripted test values, but no game action was seen producing vibration yet. Gyro was tested only with a scripted route, because no test pad reports a gyro |
| Frame rate | Works | 60 fps by default, like the original; the targets above 60 are listed under Features. A Florio ride held 60.0 fps on an RTX 4090. Frame rate drops when several test instances share one PC; that case has no clean benchmark yet |
| Vulkan validation | Works | Title, hub, Florio, Reef, Jungle, Beach and Snowfields rides and photo select gave no synchronization hazards and no other validation errors after one startup pipeline error was fixed |

**Known problems**

- Reef Day shows saturated, patchy sea-bed colors. It is unproven whether that matches the original.
- Wurmple photos stay unselectable ("subject is not clearly visible"). The cause is unknown; it may be normal game behavior, since the subject is whatever the reticle center points at.
- A loading hang on Lental was seen once and did not come back in four later runs.
- Draw distance and full-rate distant animation are experimental and may change photo scores.
- A Vulkan warning remains about the BC1 thumbnail encoder drawing a float format into an RGBA16UI stand-in. The spec calls it undefined, but it works on NVIDIA.
- On a 144 Hz display a 60 FPS cadence judders. Present pacing needs VRR or frame-pacing work.
- Rumble in the game itself, evaluation score screen audio, the album viewer, the pause menu and the Sound tab volume were not checked.

## Features

### Look and graphics

- The default look is Enhanced: supersampling (2x at 1080p, 1.5x at 1440p, 1x of the output at 4K, all plus SMAA), CAS sharpening, 16x anisotropic filtering, automatic UI font supersampling, PNG photo export (lossless, with a JSON sidecar) and HD photos.
- One "Look" row (first row of the Quality tab, in the game's Settings screen and in the F1 menu) switches between Enhanced and Original. Original is the game as shipped: its own FXAA, no sharpening, no forced anisotropic filtering, no PNG files, no HD photos. The row sets all of them together; you can still change any single row afterwards. A choice you already saved keeps working: only settings you never changed follow the new default. `SNAP_VISUALS=original` does the same from the command line (see [Running and settings](#running-and-settings)).
- Graphics presets: Original, Balanced, Enhanced, Max and Custom, one row at the top of the launcher's Picture page, the F1 Graphics tab and the Quality tab of the game's Settings screen. Change any single row and it shows Custom (see [docs/presets.md](docs/presets.md)).
- Detail presets (the older "Quality preset" row): Very Low, Low, Default (follows the Look), High, Ultra, Maximum and Custom.
- Upscalers: NVIDIA DLSS and DLAA, AMD FSR and Intel XeSS, each with quality modes. The vendor SDK files are not in this repo or the release zip. The builder and the launcher download them to your PC from the vendors only after you read and accept each license, and check them against pinned hashes. An upscaler that is not installed is hidden.
- HD photos (on in the Enhanced look, display only): photos taken under a scaled frame (4K, DLAA, DLSS, FSR or render scale above 1) also get a 2160x1440 copy. It is shown in the photo select preview, the score screen and the album. Turn it off with the "HD photos" row in the Photos settings tab, or choose the Original look. The game's own save, JPEG and score inputs do not change.
- Enhanced lighting, off by default ("Lighting preset" row on the F1 Graphics page and in the launcher): soft shadows that sharpen where an object touches the ground (PCSS), ambient occlusion (GTAO), contact shadows and a larger shadow map. It costs frame time at 4K. The occlusion and contact passes are skipped on photo frames.
- Ultrawide, experimental and off by default ("Ultrawide" row on the F1 Graphics page and in the launcher, restart needed): on a window wider than 16:9 the 3D view shows more of the world and the interface, menus, videos and photos stay 16:9. Photo scores have not been checked against a widened view, which is why it is off. See `docs/ultrawide.md`.
- HDR output (HDR10 and scRGB, with fallback to SDR), peak and paper-white settings.
- Render scale from 0.5x to 4x, shadow resolution up to 8192, ambient occlusion options.
- Optional color looks (Neutral, Vivid, Warm, Cool) and per-pass switches for bloom, depth of field, film grain and chromatic aberration.

### Frame rate and HD textures

- Frame rate target: 60 (the game's own), 90, 120, 144, 165 or 240 fps, under Options > Frame rate target. Menus, dialogue and the interface keep their original speed. The frame rate limit is a separate setting (Off, 30 to 240).
- Local AI HD textures: the builder upscales the textures your own copy loads, on your own GPU, and installs them as a mod pack. Levels are Off, Sharp, HD, Ultra and Custom, and F8 in game switches between original and HD. Nothing is uploaded.
- A built-in updater checks for a new release once a day. Nothing installs until you click. Every update is signed (Ed25519) and can be rolled back.

### Menus and controls

- Ten PC settings tabs inside the game's own Settings screen: Quality, Textures, Effects, Output, View, Display, Controller, Mouse, Audio and Photos. They use the game's own widgets and its Restore Default Settings. They need a local pack built from your own game files (see [Running and settings](#running-and-settings)).
- An F1 overlay (or hold Back/View + Start on a controller for half a second) with the same options, button remapping, mods and save management.
- Rumble support (host side; not yet seen in game actions) and gyro aim on controllers that report a gyro. Gyro drives the right stick because the game never reads motion.
- Keyboard and mouse, including mouse look and click-to-touch. During rides with mouse look on, the left button takes a photo and the right button zooms (`mouse.look_click_a`, `mouse.look_click_zl` in `input.json`).
- Button icons: Auto (default), Switch, Xbox or PlayStation pictures in the game's button prompts, so the prompts match your controller. The icon art is Kenney's "Input Prompts" (CC0).

### Window, saves and tools

- Borderless fullscreen (F11 or Alt+Enter), monitor choice, remembered window position and size, pause and mute when unfocused, and a fast-boot mode that skips the frame limiter during loading.
- Save import and export from the launcher or the command line: Switch save-manager backups (JKSV, Checkpoint), Ryujinx and yuzu-family save folders, and .zip files. See [Saves: import and export](#saves-import-and-export).
- Save profiles with a picker, duplicate, import and restore from trash. Each commit is an immutable generation, and the current and 5 earlier generations are kept as backups.
- One running copy per save folder, so two instances cannot corrupt a save.
- A mod loader for loose-file packs that replace single game archive entries, including HD texture packs. See [docs/mods.md](docs/mods.md). The repo holds only the loader, never pack contents made from game data.
- Options that go beyond the original game: draw distance, detail distance, full-rate distant animation, photos per expedition (72 original, up to 500), NEO-ONE speed and dialogue skipping.
- Shiny Pokémon mod (experimental, off by default, cosmetic only): a small share of spawned Pokémon get a recolored texture. A row in the Effects settings tab picks Off, 1 in 4096, 1 in 512 or Test. The recolored textures are built on your PC from your own files. See [docs/shiny-mod-plan.md](docs/shiny-mod-plan.md).

### Level editor (early)

A Rust course editor opens a course from your own unpacked files and draws its geometry, ride rail, paths and spawn points. You can move and copy scenery and Pokémon spawns, bend the ride rail, and export the result as a mod pack for the mod loader. In game tests, an edited Florio with a copied Dodrio and a raised rail section rode to the end and reached photo select. New courses and new models are not supported yet. Source: [tools/level-editor](tools/level-editor). Plan: [docs/level-editor-plan.md](docs/level-editor-plan.md).

## Screenshots

Captured from the native build running on Windows. They show the running game only, no extracted files.

<table>
  <tr>
    <td align="center" width="50%">
      <img src="docs/screenshots/lab.webp" alt="Lab hub: menu list on the left, speech bubble at the bottom" width="100%"><br>
      <sub>Research lab hub</sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/screenshots/florio-ride.webp" alt="Dodrio and Taillow beside the ride path" width="100%"><br>
      <sub>Florio Nature Park ride</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img src="docs/screenshots/beach-ride.webp" alt="Alolan Exeggutor standing by the surf" width="100%"><br>
      <sub>Blushing Beach ride</sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/screenshots/camera.webp" alt="Focus brackets around a Vivillon with its name tag" width="100%"><br>
      <sub>Camera view</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img src="docs/screenshots/photo-select.webp" alt="Grid of 18 photos from one expedition" width="100%"><br>
      <sub>Photo selection</sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/screenshots/scoring.webp" alt="Score breakdown for a Dodrio photo, total 2,422" width="100%"><br>
      <sub>Professor's evaluation</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img src="docs/screenshots/photodex.webp" alt="Bouffalant entry with its 3D model" width="100%"><br>
      <sub>Photodex</sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/screenshots/options.webp" alt="Native PC options screen, Textures tab" width="100%"><br>
      <sub>PC options</sub>
    </td>
  </tr>
  <tr>
    <td align="center" colspan="2">
      <img src="docs/screenshots/f1-menu.webp" alt="The F1 menu over a Florio ride, Graphics page: Look, Quality preset, Internal resolution, Anti-aliasing, SMAA quality, Performance scaling, Sharpening and Frame generation rows" width="60%"><br>
      <sub>F1 menu, Graphics page, open during a ride</sub>
    </td>
  </tr>
</table>

### Comparisons

Each pair is captured at the same point on the ride, so the camera matches; the Pokémon move on their own. The Florio frames for the Original look, the Enhanced look and the lighting preset come from one recent build at a 4K window, and the lighting pair is one run with the preset switched live. The Beach, lab, anti-aliasing and upscaler frames are from earlier captures. Zoom strips are 2x crops of the full 4K frame.

**Original look vs Enhanced.** Original is the game's own 1080p image scaled to 4K. Enhanced adds supersampling (sized to the output so 4K still holds its frame rate), SMAA, CAS sharpening and 16x anisotropic filtering.

<img src="docs/screenshots/compare/look-florio.webp" alt="Florio at 4K, split screen: Original look on the left, Enhanced on the right" width="100%">
<img src="docs/screenshots/compare/look-florio-zoom.webp" alt="Florio foliage at 2x: stair-stepped leaf edges in Original, clean edges in Enhanced" width="100%">
<sub>Florio: Enhanced removes the stair steps on grass and leaf edges and sharpens the ground.</sub>

<details>
<summary>Blushing Beach and the lab</summary>

<img src="docs/screenshots/compare/look-beach.webp" alt="Blushing Beach at 4K, split screen: Original on the left, Enhanced on the right" width="100%">
<img src="docs/screenshots/compare/look-beach-zoom.webp" alt="Beach flowers at 2x, Original and Enhanced" width="100%">
<sub>Blushing Beach: clean leaf and petal edges with Enhanced.</sub>

<img src="docs/screenshots/compare/look-lab.webp" alt="The lab at 4K, split screen: Original on the left, Enhanced on the right" width="100%">
<img src="docs/screenshots/compare/look-lab-zoom.webp" alt="Lab rope fence and doorframe at 2x, Original and Enhanced" width="100%">
<sub>The lab: thin lines like the rope fence stay solid with Enhanced.</sub>

</details>

**Enhanced lighting.** The Enhanced look with the lighting preset off and on, at the same point of the ride. The occlusion shows between the leaves and under the trees.

<img src="docs/screenshots/compare/lighting-florio.webp" alt="Florio at 4K, split screen: lighting preset off on the left, on on the right" width="100%">
<img src="docs/screenshots/compare/lighting-florio-zoom.webp" alt="Grass at 2x: flat shading with the lighting preset off, shaded depth between the blades with it on" width="100%">

**Ultrawide.** A 3440x1440 window at Florio, with Ultrawide off (top, 16:9 with side bars) and Auto (bottom). The view is wider, and the buttons and the camera frame stay where they were. In the Auto frame the film counter's number sits at the far right edge of the window, away from its icon.

<img src="docs/screenshots/ultrawide.webp" alt="Florio ride in a 3440x1440 window: Ultrawide off with black side bars on top, Auto with a wider 3D view below" width="100%">

**Anti-aliasing at native 4K.** The game's own AA, SSAA + SMAA (the Enhanced default) and DLAA.

<img src="docs/screenshots/compare/aa-native.webp" alt="Florio at 4K with the game's AA, SSAA plus SMAA, and DLAA" width="100%">
<img src="docs/screenshots/compare/aa-native-zoom.webp" alt="Foliage at 2x with the game's AA, SSAA plus SMAA, and DLAA" width="100%">

**Upscalers.** DLSS, FSR and XeSS in Quality mode: 1440p internal, 4K output.

<img src="docs/screenshots/compare/upscalers.webp" alt="Florio at 4K output with DLSS, FSR and XeSS Quality" width="100%">
<img src="docs/screenshots/compare/upscalers-zoom.webp" alt="Grass at 2x with DLSS, FSR and XeSS Quality" width="100%">

## Linux AppImage (v1.2.0)

This fork distributes **Linux x86_64 AppImage** releases only (`New-Pokemon-Snap-Linux-v1.2.0-x86_64.AppImage`). Windows builds remain maintained by `finalpantasy` in the [original repository](https://github.com/finalpantasy/new-pokemon-snap-recomp).

> [!IMPORTANT]
> **You must supply your own legally dumped copy of New Pokémon Snap (version 2.0.1 / 262144).** The AppImage contains no ROMs, keys, extracted assets, or proprietary game code.

### System requirements
- **Minimum:**
  - 64-bit Linux (`x86_64`) with `glibc >= 2.35` (Ubuntu 22.04 LTS+, Fedora 36+, Bazzite, or newer)
  - Vulkan 1.3 capable GPU and up-to-date host driver (NVIDIA proprietary or Mesa RADV/ANV)
  - Wayland or X11 desktop session, 8 GB RAM, ~15 GB free disk space for extracted game files and caches
- **Recommended:**
  - KDE Plasma / GNOME (Wayland) or X11
  - Dedicated GPU with 8 GB+ VRAM (e.g. NVIDIA RTX 3060 / AMD RX 6700 XT or newer)
  - 16 GB RAM and NVMe SSD

### Obtaining and configuring game files
1. Dump your own copy of **New Pokémon Snap** with the **2.0.1** update from your Nintendo Switch using your own tools (e.g., `nxDumpTool` + `hactool` / `nstool`) to obtain the patched `romfs/` directory and the `main` / `sdk` NSOs.
2. Convert/lift `main` and `sdk` to Linux AOT ELF/archive artifacts (`main.elf`, `sdk.elf`, `libsnap_main.a`, `libsnap_sdk.a`) following the build pipeline in `docs/linux-port.md` / `recomp/`.

### Running the AppImage
1. Download `New-Pokemon-Snap-Linux-v1.2.0-x86_64.AppImage` and `New-Pokemon-Snap-Linux-v1.2.0-x86_64.AppImage.sha256` from the [Releases](https://github.com/didslima/new-poke-snap-switch-recomp/releases) page.
2. Verify the checksum and make the AppImage executable:
   ```bash
   sha256sum -c New-Pokemon-Snap-Linux-v1.2.0-x86_64.AppImage.sha256
   chmod +x New-Pokemon-Snap-Linux-v1.2.0-x86_64.AppImage
   ```
3. Launch with your game files:
   ```bash
   export SNAP_ROMFS="/path/to/Pokemon Snap/romfs"
   ./New-Pokemon-Snap-Linux-v1.2.0-x86_64.AppImage \
     --main /path/to/main.elf \
     --sdk /path/to/sdk.elf \
     --main-lib /path/to/libsnap_main.a \
     --sdk-lib /path/to/libsnap_sdk.a \
     --play --fullscreen --allow-guest-exec
   ```
   Useful flags: `--version`, `--help`, `--windowed --width 1280 --height 720`, `--data-dir <dir>`, `--profile <name>`.

### Updating to future versions
Replace the `.AppImage` file with the newer release and run `chmod +x`. Your saves, settings, and shader caches are stored outside the AppImage and are preserved across updates.

### Where saves, settings, and caches are stored
- **Saves, profiles, screenshots, `input.json`, `window.json`:** `$XDG_DATA_HOME/SnapRecomp` (default `~/.local/share/SnapRecomp`)
- **Configuration:** `$XDG_CONFIG_HOME/SnapRecomp` (default `~/.config/SnapRecomp`)
- **NVN Vulkan shader cache:** `$XDG_CACHE_HOME/SnapRecomp/nvn_cache` (default `~/.cache/SnapRecomp/nvn_cache`, overridable via `NVN_CACHE_DIR`)

### Common troubleshooting
- **AppImage fails to mount (`libfuse.so.2` missing):** Install `fuse` / `libfuse2` on your distribution, or run with `APPIMAGE_EXTRACT_AND_RUN=1 ./New-Pokemon-Snap-Linux-v1.2.0-x86_64.AppImage ...`.
- **Window/fullscreen issues on Wayland:** Test XWayland/X11 fallback with `SDL_VIDEODRIVER=x11`, or start in `--windowed --width 1280 --height 720`.
- **Black screen on startup:** Verify that `NVN_CACHE_DIR` points to a writable directory (`~/.cache/SnapRecomp/nvn_cache`) and, if translating shaders on first run, that `ShaderHarness` or a pre-populated `nvn_cache` is available.
- **Vulkan device not found:** Ensure your host's 64-bit Vulkan loader (`libvulkan.so.1`) and GPU ICD (`nvidia_icd.json` / `radeon_icd.x86_64.json`) are installed.

### Known Linux limitations
- Vendor upscalers (DLSS, FSR, XeSS) and frame generation are currently stubbed out on Linux (`nvn_upscale_stub.cpp`).
- The Windows GUI setup/launcher (`SnapRecomp.exe`) is not included; Linux launches via CLI / `.desktop` wrapper.
- Binaries are built against an Ubuntu 22.04 baseline (`glibc >= 2.35`, `libstdc++` `GLIBCXX_3.4.30` / GCC 11+). Distributions older than Ubuntu 22.04 LTS are not supported.

## Build it yourself

The [latest release](https://github.com/finalpantasy/new-pokemon-snap-recomp/releases/latest) is one file, `SnapRecomp.exe`. Put it in a folder of its own and double-click it. The first time it is the setup: pick your game folder and press Build, and it turns your own copy of New Pokémon Snap 2.0.1 into the game. After that the same exe starts the game. The build, your saves, settings and logs all stay in that folder, so deleting the folder removes everything ([docs/install-folder.md](docs/install-folder.md)). Setup takes a folder of extracted files (`main`, `sdk`, RomFS) or a folder with your dumped base game (XCI or NSP), the 2.0.1 update NSP and your `prod.keys`. It reads them in place and extracts only what it needs. The exe holds only our code and prebuilt runtime objects, no game data. The command-line builder is inside it: setup unpacks it to `setup\app\<version>` next to the exe. See the license and release-history notes below.

<p align="center">
  <img src="builder/release/screenshots/setup_1_welcome.png" alt="Setup wizard, welcome page" width="49%">
  <img src="builder/release/screenshots/setup_3_options.png" alt="Setup wizard, options page with the Microsoft license accepted" width="49%">
</p>

For the command-line route you need Windows 10 or 11 x64, a Vulkan 1.3 driver, Python 3.14 and about 15 GB of free disk (the one-click setup needs about 30 GB, because it also keeps the extracted game data). You do not need Visual Studio: `python builder\snap_builder.py winsdk --accept-license` fetches Microsoft's C runtime and Windows SDK files with a pinned xwin, and `build --winsdk auto|vs|xwin` picks between that and Visual Studio Build Tools 2022 when it is installed. You also need the game's `main` and `sdk` files and the patched RomFS folder, extracted from your own copy with your own tools.

1. `python builder\snap_builder.py check --main <main> --sdk <sdk> --romfs <romfs folder>`
2. `python builder\snap_builder.py fetch llvm ghidra switch_loader jdk`
3. `python builder\snap_builder.py pyenv`
   Without Visual Studio Build Tools, also run `python builder\snap_builder.py winsdk --accept-license` once.
4. `python builder\snap_builder.py build --main <main> --sdk <sdk> --romfs <romfs folder> --work <new empty folder>`
5. Double-click `<work folder>\out\SnapRecomp.exe`.

Other launchers can do all of this without our windows: `SnapRecomp.exe --game <folder> --yes` builds, `SnapRecomp.exe --play` starts the game, and every launcher option has a flag ([docs/command-line.md](docs/command-line.md)).

The first build takes 25 to 50 minutes, most of it Ghidra and compiling. The first start translates shaders and takes several minutes.

The command-line route runs in a terminal; the one-click setup needs none. The finished `SnapRecomp.exe` opens its own launcher window: Play, settings, saves, mods, HD textures, Add to Steam, updates and the game folder.

<p align="center">
  <img src="docs/screenshots/launcher.webp" alt="The Snap Recomp launcher home page: the Play tile, status chips, a list of pages and the loadout card with Look, Window, Frame pacing, Resolution, Anti-aliasing, Quality and Sound" width="49%">
  <img src="docs/screenshots/launcher-settings.webp" alt="The launcher's Picture settings page: Look, Internal resolution, Anti-aliasing, SMAA quality, Performance scaling, Sharpening and Quality preset" width="49%">
</p>

### HD textures

`python builder\snap_builder.py hdtex --collect <out folder>\SnapRecomp.exe`, then `python builder\snap_builder.py hdtex --level hd`. Levels are Off, Sharp, HD, Ultra and Custom. Upscaling runs on your GPU. Press F8 in game to switch between original and HD.

The updater checks once a day and installs only when you click. Turn the check off under Updates in the launcher. See [builder/README.md](builder/README.md) for every option.

## Building

You need your own dump of the game. Nothing here downloads it, and no script reads from anywhere except your own files.

<details>
<summary><b>Prerequisites</b> (tools and paths)</summary>

Windows 10 or 11 x64. Local tools go under `tools/`, which is git-ignored, and the scripts look for them at these paths.

| Tool | Path | Used for |
| --- | --- | --- |
| LLVM, clang and lld 23 | `tools/llvm` | Building the lifted C and the host |
| Visual Studio 2022 Build Tools | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` | Windows SDK and `vcvars64.bat` |
| Vulkan SDK | `tools/vulkan_sdk` | `vulkan-1.lib`, `glslangValidator` |
| SDL3 (optional) | `tools/dl_sdl3` | Gamepads. XInput is the fallback |
| Python 3 | `tools/venv-recomp` | Lifter and build scripts. Needs `capstone`, `pyelftools`, `numpy`, `lz4` (NSO conversion). `unicorn` for the instruction tests |
| Python 3 | `tools/venv-oodle` | DRP unpacking. Needs `unicorn` for the in-engine Oodle decoder |
| Rust | none | Building the XOR helper: `rustc -O --crate-type cdylib -o tools/oodle_build/drp_xor.dll scripts/drp_xor/drp_xor.rs` |
| NSTool | `tools/nstool` | NCA and RomFS extraction |
| Ghidra 12.1.2 with the Switch loader extension, JDK 21 | `tools/ghidra_*`, `tools/jdk-*` | Function discovery |
| .NET 10 SDK | none | `scripts/shader_harness`, which the host build needs. It references a manually cloned checkout of the Ryubing Ryujinx shader translator at `tools/ref/ryubing-ryujinx` |

`scripts/audit_revisions.py` is the only script that reads `SNAP_DUMPS_DIR` (default `./dumps`): set it to the folder holding your XCI/NSP files and `prod.keys`. NSTool needs `prod.keys` in one of its documented key locations.

</details>

### Steps

Run from the repo root.

1. **Extract.** Use NSTool on the base XCI plus the 2.0.1 update NSP to get the program NCA and the patched RomFS (into `extracted/`). `scripts/pfs0.py` lists and extracts PFS0/NSP members.
2. **Convert modules.** `python -I scripts/nso2elf.py <main.nso> <main.elf>`, then the same for `sdk`.
3. **Unpack archives.**
   ```powershell
   tools\venv-oodle\Scripts\python -I scripts\drp_unpack.py --set priority
   ```
   It is resumable and writes only to `extracted/`.
4. **Discover functions.**
   ```powershell
   scripts\ghidra_import.ps1 -InputFile <main.elf> -Project snap-v262144
   scripts\ghidra_analyze.ps1 -Stage All
   ```
   `-Stage All` runs Analyze, Export and Reconcile. Reconcile writes `functions_final.tsv` and `jump_tables.tsv`, which the lifter needs.
5. **Lift and compile.**
   ```powershell
   tools/venv-recomp/Scripts/python -I recomp/snap_gen.py --module main
   tools/venv-recomp/Scripts/python -I recomp/snap_gen.py --module sdk
   tools/venv-recomp/Scripts/python -I recomp/snap_build.py
   ```
   Generated C goes under `generated/`, which is never committed.
6. **Build the shader harness.**
   ```powershell
   dotnet build scripts/shader_harness/ShaderHarness.csproj -c Release
   ```
   `scripts/xlate_build.ps1` does the same build. The output goes to `tools/shader_harness/bin`, and the host build bakes in the DLL path.
7. **Build the host.**
   ```powershell
   tools/venv-recomp/Scripts/python -I host/build_snap.py
   ```
   `build_snap.py` also runs `host/gen_nvn_bridge.py` and `runtime/hle/gen_hle_stubs.py` to produce the NVN bridge and the unimplemented-function stubs. It produces `build/snap/snap_boot.exe`. A cold host build takes about 90 seconds, and a relink 5 to 10 seconds.
8. **Run.** See the next section.

`AGENTS.md` and `host/README.md` hold the detailed flags and the current state of each step.

## Running and settings

```powershell
powershell -File host\play.ps1                 # normal play, no timeout
powershell -File host\play.ps1 -SkipIntro      # feeds a script through the intro (about 3 to 4 minutes), then hands control back
powershell -File host\play.ps1 -ListPads       # list detected controllers
powershell -File host\run_boot.ps1 -Timeout 100 run   # timed, hidden run with a trace log
```

`play.ps1` also takes `-Profile`, `-PickProfile` and `-NoNativeOptions`. The game window needs one click for focus. Saves are under `%APPDATA%\SnapRecomp\`.

`play.ps1` builds a local `native-options` pack from your own extracted `option.lmb` before each start (`python -I scripts/ui/make_native_options.py`). The pack is game-derived and is never committed. If the build fails, the PC tabs do not appear. In the game, open the Settings screen from the lab hub gear and press R past the last tab, "Other".

<details>
<summary><b>Environment variables</b></summary>

A value set by hand always wins over a menu choice and over `SNAP_VISUALS`.

| Variable | Values | Effect |
| --- | --- | --- |
| `SNAP_VISUALS` | `enhanced` (default), `original` | Switches the visual defaults together (anti-aliasing, sharpening, anisotropic filtering, UI font, PNG export, HD photos). See `host/README.md` for the table |
| `SNAP_PROFILE` | save profile name | Picks the save profile, the same as `play.ps1 -Profile "<name>"` |
| `SNAP_RETURN_TO_LAUNCHER` | `1` | After a game started from the launcher window closes, open the launcher again instead of ending. The Display page of the launcher has the same switch (key `return_to_launcher`) |
| `SNAP_AA` | `original` (default), `fxaa`, `off`, `smaa`, `ssaa2`, joined with `+` or `,` | Anti-aliasing, for example `ssaa2+smaa` |
| `SNAP_RENDER_SCALE` | number | Explicit render scale, never capped |
| `SNAP_SHARPEN` | 0 to 1 | CAS sharpening strength |
| `SNAP_SMAA_QUALITY` | `low`, `medium`, `high` (default), `ultra` | SMAA preset: edge threshold, search steps, corner rounding (the reference presets) |
| `SNAP_ANISO` | `0` (default, the game's own) or `1` to `16` | Anisotropic filtering |
| `SNAP_UPSCALER` | `off`, `dlss`, `dlaa`, `fsr`, `xess` | Upscaler backend. Falls back through DLSS, FSR and XeSS |
| `SNAP_UPSCALE_QUALITY` | `native`, `quality`, `balanced`, `performance` | Upscaler quality |
| `SNAP_HDR` | `off`, `hdr10`, `scrgb` | HDR output. Peak and paper white (in nits) use `SNAP_HDR_PEAK_NITS` and `SNAP_HDR_PAPER_NITS` |
| `SNAP_PHOTO_EXPORT` | 0, 1 | PNG copies of photos. The folder is `SNAP_PHOTO_DIR` |
| `SNAP_UI_FONT_SCALE` | `off`, `auto`, 1 to 4 | Sharper menu text. `auto` gives 2x at 1440p and above |
| `SNAP_FAST_BOOT` | 1 | Turns the frame limiter off during loading (about 28% faster to the title logo) |
| `SNAP_MODS_DIR` | path | Mod pack folder |
| `SNAP_CONFIG_DIR` | path | Where `input.json` and `window.json` live |

</details>

## Saves: import and export

Bring a save in, or take yours out, with the launcher buttons "Import save..." and "Export save...", or from the command line. Import writes the files as a new save generation, so the save you had stays as a backup. It refuses while the game is running.

- **Import from:** a folder of the game's save files, or a .zip of one. Backups from a save manager such as JKSV or Checkpoint work, and so do the save folders of Ryujinx and the yuzu family of emulators. One level of nesting is fine: a dated backup folder, or Ryujinx's `0` folder (its `1` folder is ignored). The launcher also lists emulator saves it finds on this PC, newest first.
- **Checks:** all 15 save files must be present with the right names and sizes. A save from another game, an incomplete copy or a damaged zip is refused with a message, and nothing changes. A save from an older game version imports with a warning, since the game upgrades it on the first load.
- **Export:** copies the current save's files into a folder you pick (into a dated subfolder when that folder is not empty), or into a .zip when the path ends in `.zip`. The zip has the files at its top level, the layout JKSV uses. Restore the result with a save manager, or copy its files into an emulator's save folder for this game.
- **Command line:** `snap_boot.exe --import-save <folder or .zip> [--profile <name>]` and `snap_boot.exe --export-save <folder or .zip>`. The exit code is 0 for success, 1 for a failure and 2 when the game is running.

## Testing

<details>
<summary><b>Test suites and scripted routes</b></summary>

- `tests\hle\run_tests.ps1`: HLE host-side tests (build and run).
- `tests\mods\run_tests.ps1`: mod loader tests.
- `tests\os\build.bat`, `tests\audio\build.bat`, `tests\nvn\build.bat`: OS, audio and NVN tests.
- `tests/insn/`: instruction-level differential tests against a Unicorn reference.
- `tests\hle\run_ui_test.ps1`, `run_options_test.ps1`, `run_profiles_test.ps1`, `run_switch_test.ps1`: tests that boot the game.

Game-level checks use scripted routes: `powershell -File host\run_boot.ps1 -Route <name>` feeds `host\routes\<name>.txt` as input and saves checkpoint screenshots. Scripts can wait on game state with `wait <condition>` and `end` lines (conditions such as `ui=<scene>`, `dialog`, `ride`, `ride_end` and `film<=<n>`), so a route does not depend on fixed timing. A timeout exits with code 3 and logs the UI state. Up to 4 hidden runs at a time; `build\slots\max.txt` can lower that cap. See `host/README.md` for details.

</details>

## Project layout

| Path | Contents |
| --- | --- |
| `recomp/` | Python lifter and C emitter. Reads the game's `main` and `sdk` modules as ELF files and writes C, one function per guest function |
| `runtime/` | Guest CPU model and SIMD helpers, Horizon OS layer (`os/`), HLE services (`hle/`), audio (`audio/`, WASAPI), NVN-to-Vulkan graphics (`nvn/`), mod loader (`mods/`) |
| `host/` | Loader, boot driver, `build_snap.py`, `play.ps1`, `run_boot.ps1`, scripted routes and in-game mods |
| `scripts/` | Extraction, unpacking and analysis tools: NSO to ELF, PFS0, DRP archives, shader and format parsers, Ghidra scripts, an offline Maxwell-to-SPIR-V harness |
| `tests/` | Instruction differential, HLE, mods, audio, NVN and OS tests |
| `docs/` | Design notes, `mods.md`, the HD, shiny and level editor plans, and screenshots |
| `AGENTS.md` | The project's decisions and current state |

Game shaders are translated offline (Ryujinx shader translator to SPIR-V) and loaded at run time from your own extracted files.

## Roadmap

In progress: save hardening and the profile changes it needs, load-time and scheduler fixes, and NVN object lifetime fixes and audits.

Planned:

- Scoring the two Illumina entries that the blind route cannot photograph.
- A human play-through. Professor requests, the album viewer and several secrets are unchecked.
- The cause of the Wurmple photo problem.
- Present pacing for high-refresh displays.

## FAQ

**Is this an emulator?**
No. The game's code is translated ahead of time into C and compiled into a Windows executable. The Switch OS and GPU interfaces are re-implemented natively (Horizon OS services, and NVN on top of Vulkan). There is no CPU emulation loop at run time.

**Where are the game files?**
Dump your own copy of version 2.0.1 and run the pipeline above. Generated code, extracted assets, keys and vendor SDK binaries are git-ignored and must never be committed or shared.

**Does it change photo scoring?**
No. The Enhanced look changes only how the picture is drawn and what extra files are saved; the game's own photo, save and score data are the same as with the Original look. Photos and scoring never use jittered or upscaled output. The View options (draw distance, detail distance, full-rate animation) can change which Pokémon are in a photo and so the score, which is why they are marked experimental.

**Why does the first launch build something with my game files?**
The native settings tabs are patched into a copy of the game's own settings UI file, built on your PC. That file is never committed.

## License and credits

This unofficial Linux port is based on **[finalpantasy/new-pokemon-snap-recomp](https://github.com/finalpantasy/new-pokemon-snap-recomp)**, created and maintained by **`finalpantasy`**.

GPL-3.0 with the section 7 DLSS/Streamline/XeSS additional permission, see [LICENSE](LICENSE). Releases up to and including v1.1.0 were MIT-licensed. Their MIT text is preserved in [LICENSE-MIT](LICENSE-MIT); existing MIT-covered components retain their notices. The lifter and CPU runtime adapt TotK Recomp and LAS Recomp (both MIT); the shader harness builds on the Ryujinx shader translator; the SSBH layout follows ssbh_lib. The project logo is original artwork; its lettering is drawn from the Rubik typeface (SIL Open Font License 1.1). Full notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

New Pokémon Snap is a trademark of Nintendo and The Pokémon Company. This project is unaffiliated with them.
