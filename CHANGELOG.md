# Changelog

Unofficial controls fix, on top of [NaGaa95/gtalcs_nx](https://github.com/NaGaa95/gtalcs_nx).

All findings below came from disassembling the retail `libGame.so` (ARM64) and
cross-referencing the engine's own functions. Nothing here is guesswork about what
the buttons "should" do — each claim is traced to a specific function and field.

## 1.0.3+r3

### Added

**Liberty Menu**, an in-game mod menu on **Minus**. Native throughout: no CLEO, no
scripts, nothing from anyone else's mod. Every entry calls a function the retail
binary already exports, found by disassembling it.

* **Cheats** - the game's own cheat functions, by category. The list is checked
  against the retail button codes rather than the symbol names, which caught real
  mistakes: "peds have weapons" is `WeaponsForAllCheat`, not
  `DoChicksWithGunsCheat`; "cars drive on water" is `BackToTheFuture`, which
  toggles `CVehicle::bHoverCheat`. Cheats no button code can reach - the only
  reference to each being its own definition - are not listed.
* **Teleport** - every named zone, read from `gpTheZones` and named by the game's
  own text, grouped by island; plus the marker you place on the map
  (`GRadarMap+128`, gated on the is-set byte at `+120`, so a cleared marker is
  reported rather than teleported to).
* **Spawn vehicle** - all 83 land, sea and air vehicles, enumerated by walking the
  model table and named from each model's GXT key, so nothing is hardcoded.
* **Player** - bodyguards, clear wanted level, invincible, unlimited ammo, never
  tired, never wanted.
* **Vehicle** - colours, repair, flip upright, vehicle invincible, vehicles fly.
* **Misc** - no height limit.

Two of these exist because the game offers no way back: **Normal traffic**, since
the black and white traffic cheats set a flag nothing clears, and **unlimited
ammo** restoring your original counts when switched off.

**Invincible** uses the game's own immunity byte rather than topping health up each
frame, so a single lethal hit no longer kills you. It also keeps the car you are in
above the health at which it catches fire, because an exploding car kills its
occupants outright through a path no flag gates.

### Changed

* **`key_minus` defaults to `NONE` instead of being forced to it.** The reason for
  the default is unchanged - any other binding fires a game action at the moment
  the menu opens - but `config.txt` can now override it. Overriding a setting the
  user can see and edit was the wrong call.
* **New icon.**

### Fixed

* `DEBUG_LOG` is off for release, and its guards now honour the value. Every one
  was `#ifdef DEBUG_LOG`, which is true for `#define DEBUG_LOG 0` as well, so
  setting it to zero looked like disabling logging and did nothing.


## 1.0.3+r2

### Changed

* **config.txt documents itself.** Every setting is now written out with a comment
  above it explaining what it does, and the button section lists every valid action
  name inline. Editing the file by hand no longer means looking anything up.
  `read_config()` already skipped `#` lines, so only the writer changed.

  This replaces r1's behaviour of recording only settings that differ from the
  defaults. An option that is never written cannot be commented, so the full list is
  written out instead. Existing values are preserved on first launch; the file just
  gets longer.

* **The four face buttons are written commented out** while they still match what
  `psp_layout` resolved to, for example `#key_a B`. They have no fixed default —
  `config_resolve_faces()` only fills them when config.txt does not name them — so
  writing them as live lines would pin them and make `psp_layout` appear to stop
  working on the next launch. Commented, they show what the layout chose and are
  ready to uncomment; a remap actually chosen differs from the default and is
  written as a live line.

### Removed

* **CONFIG.md.** Its contents now live in config.txt itself, so the reference and
  the file being described can no longer drift apart.

## 1.0.3+r1

### The root cause

`CPad::Update()` copies the JNI gamepad button ids into `CControllerState` fields
using a fixed table, and that table does **not** match the id names the port
inherited from the Android keycode list:

| JNI id sent | lands on field | what actually reads it |
|---|---|---|
| `L2` (8) | `0x12` | **logical d-pad Up** — camera cycling, horn, sniper zoom, `GuiUp`, menus |
| `R2` (9) | `0x14` | **logical d-pad Down** — camera, horn, free aim, `GuiDown` |
| `DPAD_LEFT` (10) | `0x16` | d-pad Left — weapon cycle, radio down |
| `DPAD_RIGHT` (11) | `0x18` | d-pad Right — weapon cycle, radio up |
| `DPAD_UP` (12) | `0x2e` | **nothing at all** |
| `DPAD_DOWN` (13) | `0x30` | only `CCam::Process_FollowCar_SA`'s recentre |

The port was sending physical d-pad Up/Down as ids 12/13, which land on fields no
code reads — so they did nothing — while ZL/ZR were sending ids 8/9 *on top of*
their trigger axes, driving the d-pad actions by accident.

This is confirmed three independent ways: the field table itself, the observed
behaviour (ZR audibly sounded the horn while looking right), and `CPad::DoCheats()`,
which emits one named character per button — T for Triangle, C for Circle, S for
Square, X for Cross, U/D/L/R for the d-pad, 1 for L, 2 for R — so the game names
every field itself.

### Fixed

* **D-pad Up/Down now work.** They reach the engine's logical d-pad fields. This
  alone restores camera cycling, the horn, sniper/scope zoom, `GuiUp`/`GuiDown`,
  and d-pad navigation in the pause menu.
* **Cheat codes work.** Most LCS cheats contain Up or Down, so they were impossible
  to enter before. All 46 are now reachable.
* **ZL/ZR no longer double-fire.** They previously sent both a trigger axis and a
  button id, which is why ZR looked right *and* sounded the horn at the same time.
  They now send the axis only: ZL = look left, ZR = look right, both = look behind
  in a vehicle, ZR alone = look behind on foot.
* **R + D-pad Down free aim** fires, since it needs the d-pad Down field.
* **`CPad::m_bSwapNippleAndDPad` is pinned off.** This on-screen-controls option
  selects which pair of `CControllerState` fields every d-pad accessor reads. Set,
  it points them at fields only the Android touch overlay writes — killing the
  entire d-pad. A save made on a phone with that option enabled would have done
  exactly that. It is re-asserted each frame, since `LoadSaveData()` restores it
  from the save file.

### Added

* **Per-button remapping.** Every physical Switch button can be pointed at any
  engine action from `config.txt` (`key_a`, `key_zl`, `key_lstick`, ...). Upstream
  only had the `xbox_layout` face-button toggle.
* **PSP face-button layout by default** (`psp_layout 1`): positional, so the shapes
  land where a PSP player expects them — X = Triangle (top), Y = Square (left),
  A = Circle (right), B = Cross (bottom). Printed cheat codes can be entered by
  shape. `psp_layout 0` restores the Nintendo confirm/cancel convention.
* **`HORN` action, default L3.** The horn shares its field with the d-pad, so no
  button id can sound it alone; a hook on `CPad::GetHorn` gives it a dedicated
  button. The d-pad horn still works. Matches the PS2 release, which puts the horn
  on L3. Mappable to any button with `key_* HORN`. Vehicles only — `CPad::GetHorn`
  is called from the vehicle control code, so it does nothing on foot.
* **R3 recentres the camera behind a car**, using an id that had a reader but no
  button.

### Changed

* **Config action names corrected.** `key_up DPAD_UP` instead of the misleading
  `key_up L2`. `L2`/`R2` still parse as the old spellings and are rewritten to the
  new names, so existing config files keep working.
* **config.txt only records settings that differ from the defaults.** A stock file
  is four lines instead of twenty-one. A remap you actually chose still survives the
  rewrite on boot; a line restating a default is dropped.
* **`xbox_layout` renamed to `psp_layout`.** It always meant this same positional
  mapping. The old name is still parsed, but is no longer written back — an old
  config would otherwise keep pinning the Nintendo layout with no way to notice.

### Known limitations

Not bugs — the engine has no code behind them:

* **Select does nothing.** On the PSP it was Camera Modes, but the mobile build
  moved that to the d-pad and `CPad::CycleCameraModeJustDown()` has zero callers.
* **L3/R3 have no engine bindings.** The PSP has no stick clicks, so the `THUMBL`
  and `THUMBR` ids write fields nothing reads. This is why the horn needed a hook
  rather than a mapping.
* **The PS2 layout can't be reproduced.** It puts look-behind on R3 and the horn on
  L3; this build is the PSP/mobile one and has neither binding.
* **Some in-game hint text uses PS2 button names** ("Press R2 to look behind").
  On a Switch that is ZR.
* **Alternate control schemes exist in the engine but cannot be selected.**
  `CPad::GetHandBrake` and `CPad::GetHorn` branch on a stored control-scheme number,
  and on some values the hand brake becomes a Cross + Square combo instead of R.
  This build has no Controller Setup option in its menus, so the value never changes
  and the hand brake stays on R (confirmed on hardware).
* **Three cheat sequences do nothing.** They are recognised, but their handler
  clears the buffer and returns without calling anything — leftovers disabled in
  this build. They are listed in the README for completeness.
