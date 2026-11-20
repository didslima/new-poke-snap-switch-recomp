# Steam Deck preset

A handheld profile for the Steam Deck's 1280 x 800 screen, played with the Deck's own buttons.

Turn it on with the flag `--steam-deck` (for example as the launch option of the Steam shortcut: `SnapRecomp.exe --play --steam-deck`), or in the launcher on the Display page with the Device row. Both write or apply the same table (`host/steamdeck.h`).

| Setting | Value | Why |
| --- | --- | --- |
| Window | borderless 1280 x 800 | The Deck's panel. The 16:9 picture sits inside it with thin bars. |
| Internal resolution | 0.75 (1440 x 810) | Lighter than the game's 1920 x 1080 and still sharper than the panel. |
| Anti-aliasing | SMAA, medium | Cheap edge smoothing. |
| Anisotropic filtering | 4x | Light on the handheld GPU. |
| Sharpening, bounce light | off | Nothing extra. |
| Frame rate, frame generation, ray tracing | the game's 60 fps, off, off | Battery and heat. |
| HUD scale | 150% | The viewfinder and dialogue read at arm's length. |
| Menu text | 2x sharpness | Matches the bigger HUD. |
| Button icons | Xbox | Steam Input shows the Deck as an Xbox pad, so prompts never show key caps. |

On the command line the flags after `--steam-deck` win, so `--steam-deck --set hud_scale=200` gives a 200% HUD. In the launcher, changing any of these rows afterwards shows Device as Desktop again; picking Desktop puts the rows back to their defaults and returns to a window.

"Controller only" means the preset sets pad button prompts and a 150% HUD. The game's mouse and keyboard bindings stay available (Steam Input's desktop mode still works), and the launcher window itself is still a mouse window: start the game from the Steam shortcut with `--play`.
