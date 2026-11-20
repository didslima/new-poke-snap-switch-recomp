# Updater safety: backups, channels, portable mode, release notes

Code: `builder/snapbuild/userdata.py` (backups, portable data), `builder/snapbuild/update.py` (channels, the update and rollback flow), `host/portable.cpp` (portable start-up), `host/launcher.cpp` (Updates page), `builder/release_notes.py` and `builder/release/NOTES_TEMPLATE.md` (release notes). Tests: `builder/tests/test_userdata.py`, `test_update.py`, `test_release_notes.py`.

## Backup before every update

`update apply` and `update rollback` copy the player's data to `<data root>/update-backups/<date>_<from>_to_<to>/` after the signature check and before anything changes. The newest 3 backups are kept; older ones are deleted. The data root is `%APPDATA%/SnapRecomp`, or `<exe dir>/portable_data/Roaming/SnapRecomp` in portable mode.

What is copied:

- the current save generation of the main save and of every profile (`CURRENT`, the generation folder and its `.ok` marker); older generations are skipped because they can be several GB;
- profile lists and per-profile settings, the top-level settings files (`.json`, `.cfg`, `.ini`, `.txt`, up to 8 MB each), `config/` and `mods/mods.json`.

What is not copied: mod packs, shader and pipeline caches, logs, lock files, anything behind a link.

Rules:

- The free-space check runs first. If the drive cannot hold the copy plus 256 MB, the update stops before it touches anything and says so.
- The copy is written to a `.partial` folder and renamed when complete, so a crash never leaves a backup that looks whole.
- A backup is a valid save root. `python builder/snap_builder.py update backups` lists them. `update restore-backup [--name <backup>]` puts the settings back, and with `--import-saves` it also runs `SnapRecomp.exe --import-save` on the backup's saves (close the game first). Restoring settings keeps each replaced file as `<name>.before-restore`.
- The game's own save backups (`runtime/hle/save_backup.*`, 10 kept) are separate and unchanged.

## Release channels

Stable (default) installs only full releases. Testing also offers prereleases. The choice is the `update_channel` key in `config.json`, set from the launcher's Updates page ("Update channel") or with `snap_builder.py update check|apply --channel testing`. It survives rebuilds and updates. Switching channel clears the saved check result, because that answer belonged to the other channel. A testing build never downgrades a newer stable one: the updater only offers a version higher than the installed one.

All releases are signed the same way on both channels; a release without a valid Ed25519 signature is refused.

## Portable mode

Put an empty `portable.txt` next to `SnapRecomp.exe`. At start-up the exe points `APPDATA` and `LOCALAPPDATA` at `portable_data/Roaming` and `portable_data/Local` next to it, so settings, saves, mods, caches, logs and photo exports all stay in that folder. The real values stay available as `SNAP_REAL_APPDATA` and `SNAP_REAL_LOCALAPPDATA` (emulator save detection reads them), and `SNAP_PORTABLE=1` is set.

Updates keep the portable data: `portable.txt` and `portable_data` are renamed (not copied) into the new build folder before the switch, and back if the switch fails, so they never end up in `builds/previous`. The updater's own state lives in `<work>/updates`, beside `out/`, because `out/` is swapped during an update.

Rollback also puts back the exe you start the game with. An update copies the exe it replaces to `<updater state>/exe/previous.exe`; `update rollback` swaps that copy with the exe in use, so the exe always matches the active build, and a second rollback goes forward again. An install updated before this was added has no kept exe: the rollback says so and leaves the exe alone.

Not covered: the builder's work folder and the extracted game data are not inside `portable_data`; copying `out/` alone does not move an install to another PC.

## Release notes

Every release uses `builder/release/NOTES_TEMPLATE.md`:

1. Known issues first.
2. Saves and settings carry over.
3. Driver notes.
4. Changes, one bullet each, linked to its pull request or issue.
5. Install or update.

```
python builder/release_notes.py new --version 1.2.3 > notes.md
python builder/release_notes.py lint notes.md
python builder/release_notes.py stale --latest v1.3.0 --file old-notes.md
```

`lint` fails when a section is missing or out of order, when a change has no link into this repository, when a template placeholder is left, or when the text holds a personal detail (a user path, a name, an email). `stale` puts a banner on top of an older release that points at the latest one; running it again replaces the banner instead of stacking a second one.
