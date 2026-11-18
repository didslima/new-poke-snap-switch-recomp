# The install folder

A release is one file, `SnapRecomp.exe`. Put it in a folder of its own, on a drive with about 30 GB free, and start it. Everything it makes stays in that folder. Deleting the folder removes the game, your saves included.

## What the exe does

| State of its folder | What starting the exe does |
| --- | --- |
| No finished build | Opens the setup. It builds the game from your own game files into `work\out`. |
| A finished build | Starts the game's launcher, the same as starting `work\out\SnapRecomp.exe` yourself. |

Flags, text output and the exit code pass through to the game, so `SnapRecomp.exe --play --fullscreen` works from the top folder. Stopping the top exe stops the game it started. The flags the top exe keeps for itself are in [command-line.md](command-line.md#setup-flags).

## What is in the folder

```
SnapRecomp.exe                               the file you downloaded
setup\                                       the unpacked builder and the setup's own settings
tools\                                       the build tools setup downloads (compiler, Ghidra, JDK)
cache\                                       reusable build results
game\                                        the game data setup extracted from your own files
work\                                        the build
work\ghidra_user\                            Ghidra's own settings, cache and temp files from the build
work\out\SnapRecomp.exe                      the built game
work\out\portable_data\Roaming\SnapRecomp    saves, settings, profiles, mods, screenshots
work\out\portable_data\Local\SnapRecomp      shader cache, logs
work\updates\                                the updater's downloads and the previous version
```

Older docs name `%APPDATA%\SnapRecomp` and `%LOCALAPPDATA%\SnapRecomp`. In a release build read those as `work\out\portable_data\Roaming\SnapRecomp` and `work\out\portable_data\Local\SnapRecomp`. A development build started from a source checkout has no `portable.txt` and still uses the two Windows folders.

## Moving the folder

Close the game, move the whole folder, and start `SnapRecomp.exe` from the new place. The game corrects its saved paths on that start and prints `the game folder was moved; config.json now points at the new place`. Your game files are read where setup found them. If they sat outside the folder and you moved them too, pick the new game folder in the launcher.

## Saves from version 1.2.0 and earlier

Those versions kept saves in `%APPDATA%\SnapRecomp`. The first start of a newer version copies that folder into `work\out\portable_data` and tells you where your saves now live. It is a copy: the old folder is only read and stays as a backup. The shader cache and the updater's backups are not copied. Delete the old folder yourself once you have seen your save in the new version.

The updater in 1.2.0 cannot install a one-exe release. Download the new `SnapRecomp.exe` by hand, put it in a new folder and run setup once. An older install reuses the build tools it left in `%LOCALAPPDATA%\SnapRecomp\tools`, so they are not downloaded again.

## Shortcuts and uninstalling

Setup makes no shortcut unless you tick one on its options page. A shortcut points at the top `SnapRecomp.exe`. Setup adds no entry to Windows' installed-apps list and removes the entry an older version made.

To remove the game, delete the folder. `SnapRecomp.exe --uninstall` removes only the build and any shortcuts, and keeps your saves and the build tools.

## Updates

Updates in the launcher, or `SnapRecomp.exe --update`, downloads the new `SnapRecomp.exe`, checks its signature, rebuilds the game and then swaps the top exe for the new one. The old top exe is kept as `SnapRecomp.exe.old` until the next start. When the rebuild fails, nothing is swapped and the old version keeps working.

Started directly from `work\out`, the game still updates its own build, but it cannot replace a top exe it was not started from. Download the new one by hand in that case.
