# Changelog

Unofficial controls fix, on top of [NaGaa95/gtalcs_nx](https://github.com/NaGaa95/gtalcs_nx).

All findings below came from disassembling the retail `libGame.so` (ARM64) and
cross-referencing the engine's own functions. Nothing here is guesswork about what
the buttons "should" do — each claim is traced to a specific function and field.

## The root cause

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

## Fixed

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

## Added

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
* **[CONFIG.md](CONFIG.md)** — full `config.txt` reference: every setting, every
  action name, examples, and which action names are dead ends.

## Changed

* **Config action names corrected.** `key_up DPAD_UP` instead of the misleading
  `key_up L2`. `L2`/`R2` still parse as the old spellings and are rewritten to the
  new names, so existing config files keep working.
* **config.txt only records settings that differ from the defaults.** A stock file
  is four lines instead of twenty-one. A remap you actually chose still survives the
  rewrite on boot; a line restating a default is dropped.
* **`xbox_layout` renamed to `psp_layout`.** It always meant this same positional
  mapping. The old name is still parsed, but is no longer written back — an old
  config would otherwise keep pinning the Nintendo layout with no way to notice.

## Known limitations

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
* **Controller Setup (in-game options) rebinds things by itself.** On Setups 3 and 4
  the hand brake becomes a B + Y combo (Cross + Square) instead of R, and the horn
  swaps between d-pad Up and Down depending on the setup. That is the engine's own
  option, not the port's.
