# Lens effects: zoom bokeh and motion blur

Date: 2026-11-15. Two display-only effects in `runtime/nvn/nvn_lens_fx.cpp`, both off by default. Menu: native tab 15 "Lens" (rows "Zoom bokeh" and "Motion blur"), F1 Graphics list, `SNAP_BOKEH_DOF` and `SNAP_MOTION_BLUR` (strength; unset or 0 is off). Row list: `docs/advanced-graphics-options.md`.

## What they do

**Zoom bokeh.** While the camera is zoomed (ZL), everything away from the subject blurs into round, evenly filled bokeh discs, like a telephoto lens. Strength rises with the zoom and the effect is gone when you are not zoomed. The focus point is the subject under the reticle.

**Motion blur.** The picture blurs along the camera's motion (the NEO-ONE moving along its rail, or you turning the view), with a 1/120 s shutter at strength 1. This is camera motion applied to the depth buffer. A Pokémon's own motion is not in it: a Pokémon that runs across a still camera is not blurred.

## How it works

Both are Fx effects (`nvn_fx.cpp`): they draw into the linear HDR scene colour before the upscaler and the tonemap read it, so upscalers (DLSS, FSR, XeSS) and anti-aliasing see the blurred picture like any other scene.

- **Zoom detection.** The vertical FOV comes from the scene camera's projection matrix (the same per-view constants the upscaler reads). Measured on a Florio ride: the ride camera is 36.0 degrees vertical, ZL zooms it to 14.44 degrees (2.56x), the lab is 45 degrees and the course fly-overs run 42 to 55 degrees. So zoom = tan(18 degrees) / tan(half the FOV), a fixed 36 degree reference: wider fields read as not zoomed. (A "widest FOV since the last cut" reference was tried first and failed: a fly-over raised it to 55 degrees and the plain ride read as 1.7x zoomed.) The effect fades in between 1.05x and 1.35x. `SNAP_BOKEH_BASE_FOV=<degrees>` changes the reference, `SNAP_BOKEH_FORCE=<zoom>` forces a zoom for tests.
- **Focus.** One small pass reads a 5x5 spread of depth texels at the image centre, takes the median distance (thin leaves and sky texels do not move it), and smooths it over time in diopters (1 / distance) so the focus racks between subjects instead of jumping. A view that shows only sky focuses far away.
- **Blur.** A thin-lens circle of confusion per pixel, `coc = K * (1/focus - 1/z)`, signed (behind the focus positive, in front negative), with a small dead zone so the subject and its neighbors in depth stay sharp. Colour and CoC are made at 1/2 of the render size (1/4 at 4K and above), then a 44-tap golden-angle disc gather fills round bokeh balls, with a coverage weight so a sharp subject does not bleed into the blurred background and a blurred foreground spreads over it. A depth-aware upsample puts it back at full size. The tap pattern rotates per pixel and per frame, so the temporal upscaler averages the sampling away.
- **Motion blur.** The velocity is this frame's view-projection against the previous frame's, applied to each pixel's depth: the same reprojection the upscaler's motion vectors use (`upscale_mv.comp`), computed in the lens shader so it works without an upscaler. 14 taps along the velocity, jittered with interleaved gradient noise, with nearer taps counting for less so a close object does not smear over what is behind it. The length follows the real frame time, so the look is the same at 60, 120 or 144 fps. Camera cuts and gaps skip the blur for one frame.

## Display only: photos and scoring

Both effects are pre-tonemap Fx effects, and the Fx plumbing never runs those on an Evaluation frame (photo select and scoring, where the game renders the pictures it keeps). Since 2026-11-19 a photo taken on a ride no longer switches them off: that pass is only the 360x240 preview, a downscale of the screen, so the preview shows what the screen showed (`SNAP_FX_PHOTO_HOLD=1` restores the 3-present pause). `FxWritable` also refuses the Evaluation depth target and the photo targets. Neither effect reads or writes anything the photo or its scoring uses; both read only the scene depth and colour of the live view, and write only the live view's scene colour. So the photo is the game's sharp picture even while the bokeh is on the screen.

## Settings and test switches

| Switch | Meaning |
|---|---|
| `SNAP_BOKEH_DOF=<s>` | Bokeh strength: 0.6 Light, 1 Medium, 1.6 Strong. 0 or unset: off. |
| `SNAP_MOTION_BLUR=<s>` | Motion blur strength: 0.5 Light, 1 Medium, 2 Strong. 0 or unset: off. |
| `SNAP_LIGHT_AB=<P>` | Same-moment A/B: the effects are on in odd blocks of P presents and off in even ones (shared with the lighting stream). |
| `SNAP_BOKEH_FORCE=<zoom>` | Treat the camera as zoomed by that factor (test aid). |
| `SNAP_BOKEH_BASE_FOV=<deg>` | Fixed unzoomed vertical FOV (test aid). |
| `SNAP_BOKEH_DEBUG=1` | CoC tint (red behind the focus, blue in front) and a cyan dot at the reticle. |
| `SNAP_MBLUR_DEBUG=1` | Velocity tint. |
| `SNAP_BOKEH_K`, `SNAP_BOKEH_MAXCOC`, `SNAP_BOKEH_DEADZONE`, `SNAP_BOKEH_FOCUS_RATE` | Tuning: CoC strength, longest CoC in pixels at 1080p, dead zone, focus smoothing rate. |
| `SNAP_MBLUR_SHUTTER`, `SNAP_MBLUR_MAXLEN` | Tuning: shutter time in seconds (default 1/120), longest blur as a fraction of the picture height (default 0.07). |
| `SNAP_LENS_LOG=1` | Log the camera FOV, the zoom factor and the first effect runs. |

## Proof

Strips are in `analysis/lens/` (local, git-ignored): each `<tag>_<k>_strip.png` is Off on the left and On on the right, taken at the same moment (`SNAP_LIGHT_AB`, so the camera, the Pokémon and the lighting are identical).

- `zoomA_0..2_strip.png`, `zoomA_all.png`: zoom bokeh on the Florio ride with ZL held, three moments. The subject under the reticle stays sharp; the grass in front and the trees behind it turn into soft round discs.
- `mblur3_0..4_strip.png`, `mblur3_all.png`: motion blur with the rail at 3x speed, five moments. At the plain ride speed the 1/120 s shutter blurs only a few pixels (`mblur_*`), so the strips use 3x to make the effect readable. Mean absolute difference per pair (Off against On): 1.5, 3.5, 14.1, 22.9, 15.8. The gradient energy ratio On/Off is 1.03, 0.96, 0.60, 0.54, 1.09: the fast moments lose about 40 to 46 percent of their edge contrast, the near-still ones stay unchanged.

### Photos and scores

Evidence that scoring is unaffected, and its limit:

- By construction: the Fx plumbing skips both effects on photo frames (see "Display only"), and `PhotoGuard` in `nvn_lens_fx.cpp` would log if either effect ever ran on one.
- By log: a `florio_eval` run with both effects on reached `evaluation:Score` and `evaluation:End`. All 144 photo submits logged "Fx held" and `PhotoGuard` never fired.
- Not done: a same-photo byte comparison of the scoring input with the effects on and off (as the lighting stream did). Run-to-run score totals vary on their own, so totals alone cannot isolate the effects.

## Cost

GPU time per effect run, measured with `SNAP_LENS_TIME=1` at 3840x2160 with `SNAP_RENDER_SCALE=2` on a shared GPU (not an exclusive `-Bench`):

| Effect | Average | Worst frame | Notes |
|---|---|---|---|
| Zoom bokeh | 0.33 ms | 1.9 ms | Averaged over the zoomed frames; the effect is off when not zoomed. |
| Motion blur | 0.39 to 0.42 ms | about 3 ms | The 3x-speed run averaged 0.55 to 0.61 ms because the blur lengths are longer. |

## Limits

- Motion blur follows the camera only. A Pokémon that moves against a still camera is not blurred.
- No exclusive `-Bench` run: the milliseconds above come from GPU timestamps on a GPU other runs were also using, so treat them as an upper bound.

## Regenerating the proof

```
powershell -File scripts\lens\run_lens_compare.ps1 lens_zoom <even presents> build\w\<owner> dof
python -I scripts\lens\strip_lens.py "analysis\boot\runs\<id>\route_lens_zoom_*_NN.png" <tag>
```

The route `host/routes/lens_zoom.txt` rides Florio with a long ZL hold. The strips land in `analysis/lens/` (local, git-ignored).
