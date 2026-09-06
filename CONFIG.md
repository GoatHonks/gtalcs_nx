# config.txt reference

`config.txt` lives at `/switch/gtalcs/config.txt` and is created the first time you
run the game. It is optional — every setting below already has a sensible default.

Format is one `name value` per line. Blank lines are ignored and lines starting
with `#` are comments.

```
# force 720p even when docked, and show the FPS counter
screen_width 1280
screen_height 720
show_fps 1
```

**The file is rewritten on every boot, and only settings that differ from the
default are kept.** A stock file is just four lines. If you add a line that
matches the default it disappears again; delete a line to go back to default.
Nothing is lost by that — the defaults live in the code, not in this file.

## Display

| Setting | Values | Default | What it does |
|---|---|---|---|
| `screen_width` | pixels, or `-1` | `-1` | Render width. `-1` = 1280 handheld / 1920 docked. |
| `screen_height` | pixels, or `-1` | `-1` | Render height. `-1` = 720 handheld / 1080 docked. |
| `trilinear_filter` | `0` / `1` | `1` | Trilinear texture filtering. `0` is slightly sharper and slightly faster. |
| `show_fps` | `0` / `1` | `0` | Small FPS counter in the top-left corner. |

## Controls

| Setting | Default | What it does |
|---|---|---|
| `psp_layout` | `1` | `1` = face buttons positional, matching the PSP: X = Triangle (top), Y = Square (left), A = Circle (right), B = Cross (bottom). `0` = Nintendo confirm/cancel convention (A = Cross, B = Circle). Also accepted under its old name `xbox_layout`. |

### Remapping a button

One `key_*` per physical Switch button. The value is an **engine action**, not
another button.

| Setting | Physical button | Default action |
|---|---|---|
| `key_a` `key_b` `key_x` `key_y` | face buttons | from `psp_layout` |
| `key_l` / `key_r` | L / R | `L1` / `R1` |
| `key_zl` / `key_zr` | ZL / ZR | `NONE` (see note below) |
| `key_up` `key_down` `key_left` `key_right` | D-pad | `DPAD_UP` / `DPAD_DOWN` / `DPAD_LEFT` / `DPAD_RIGHT` |
| `key_lstick` / `key_rstick` | L3 / R3 (stick click) | `HORN` / `CAM_CENTER` |
| `key_plus` / `key_minus` | Plus / Minus | `START` / `BACK` |

### Action names

| Name | What it does |
|---|---|
| `A` | Cross — sprint on foot, accelerate in a vehicle |
| `B` | Circle — attack / fire weapon, car weapon |
| `X` | Square — jump, brake / reverse |
| `Y` | Triangle — enter vehicle, skip phone call |
| `L1` | L — answer phone, collect pickup, sub-mission |
| `R1` | R — target / scope view, hand brake |
| `DPAD_UP` / `DPAD_DOWN` | cycle camera, scope zoom, horn |
| `DPAD_LEFT` / `DPAD_RIGHT` | cycle weapon / target, cycle radio stations |
| `START` | pause menu |
| `HORN` | sound the horn, and nothing else |
| `CAM_CENTER` | recentre the camera behind a car |
| `BACK` | open the pause menu (Minus's default) |
| `NONE` | disable this button |
| `SELECT` | nothing — see "Dead ends" below |
| `THUMBL` / `THUMBR` / `UNUSED` | nothing — see "Dead ends" below |

`L2` and `R2` are still accepted as old spellings of `DPAD_UP` / `DPAD_DOWN`, and
get rewritten to the new names. A raw number also works if you know the id.

### Examples

```
# left-handed: swap the shoulder actions
key_l R1
key_r L1

# move the horn to R3 and free up L3
key_lstick NONE
key_rstick HORN

# Minus does nothing, pause moves to Plus only
key_minus NONE

# Nintendo face buttons instead of PSP
psp_layout 0
```

## Notes and dead ends

Two buttons deliberately send no action id:

* **ZL / ZR** are the analogue triggers. They reach the engine through the LT/RT
  axes rather than the button table, which is what drives look-left, look-right
  and look-behind. Giving them a `key_*` action **adds** that action on top —
  it does not replace the looking. Mapping them to `DPAD_UP`/`DPAD_DOWN` is
  what used to make ZR honk the horn while looking right.

Some action names exist but reach no engine code, so mapping a button to them is
the same as `NONE`:

* `SELECT` — on the PSP this was Camera Modes, but the mobile build moved camera
  cycling to the D-pad and left `CPad::CycleCameraModeJustDown()` with no callers.
* `THUMBL` / `THUMBR` — the PSP has no L3/R3, so nothing reads these. This is why
  L3 uses the special `HORN` action instead of a normal id.
* `UNUSED` — an id whose `CControllerState` field nothing reads.

## Troubleshooting

If a control stops working, check the log for these lines at boot — each one is a
hook arming itself:

```
ZOOM:    gamepad sniper/camera zoom
PAD:     d-pad field selector pinned
HORN:    dedicated horn button
FREEAIM: R + D-pad Down combo
```

`HORN:` only appears if some `key_*` is set to `HORN`. If you set every button
away from `HORN`, the hook is skipped and the horn stays on the D-pad only.

To start over, delete `config.txt` — it is rebuilt with the defaults on next boot.
