<div align=center>

<img src="extras/banner.png" alt="Banner" width="50%">

</div>
<h1 align=center>GTA: Liberty City Stories - Nintendo Switch port</h1>

This is a wrapper/port of the Android version of Grand Theft Auto: Liberty City Stories (v2.4.379).
It loads the original game binary, patches it and runs it.
It's basically as if we emulate a minimalist Android environment in which we natively run the original Android binary as is.

### About this fork

This is an unofficial fork of [NaGaa95/gtalcs_nx](https://github.com/NaGaa95/gtalcs_nx)
with a set of controller fixes. **All credit for the port itself goes to NaGaa95**,
and to Andy Nguyen and fgsfds, whose loader it is built on. I did not write the port
— this fork only changes how controller input reaches the engine.

I am not a developer and know very little programming. The changes here were made
entirely with [Claude Code](https://claude.com/claude-code); my part was describing
the problems I ran into while playing and testing the results on hardware. The
reasoning behind each change is in [CHANGELOG.md](CHANGELOG.md) so anyone can check
it, and the code comments explain what was found in the game binary and why.

If you hit the same problems I did — the d-pad doing nothing, cheat codes not
working, ZR sounding the horn while looking right — this should fix them.

**This release contains no game files.** You still need your own legally obtained
copy of the Android APK (version 2.4.379); see the install instructions below.

### How to install

You're going to need:
* the `.apk` for version **2.4.379**

To install:
1. Create a folder called `gtalcs` in the `switch` folder on your SD card.
2. Extract `assets/data_main.wad` and `assets/data_music.wad` to `/switch/gtalcs/`.
3. Extract `lib/arm64-v8a/libGame.so` **and** `lib/arm64-v8a/libopenal.so` from the `arm64-v8a`
   APK to `/switch/gtalcs/`.
4. Extract `res\raw\intro.m4v` to `/switch/gtalcs/`.
5. Copy `gtalcs_nx.nro` into `/switch/gtalcs/`.

Your SD card should contain: `/switch/gtalcs/gtalcs_nx.nro`, `/switch/gtalcs/libGame.so`,
`/switch/gtalcs/libopenal.so`, `/switch/gtalcs/data_main.wad` `/switch/gtalcs/data_music.wad`
and `/switch/gtalcs/intro.m4v`

### Notes

This will not work in applet/album mode. Use a game override (hold R on a title) or a forwarder.

Save games and settings are stored in `/switch/gtalcs/`.

The port has an extra config file at `/switch/gtalcs/config.txt`, created the first
time you run the game. See **[CONFIG.md](CONFIG.md)** for the full list of `config.txt` settings, every
action name, and examples. config.txt is optional and only records settings that
differ from the defaults, so a stock file is four lines.

## Controls

The PSP layout, verified against the engine rather than guessed: `CPad::DoCheats()`
feeds a character to `CPad::AddToCheatString()` for each button, which names every
`CControllerState` field for certain (`T`riangle, `C`ircle, `S`quare, `X` = Cross,
`U`p, `D`own, `L`eft, `R`ight, `1` = L, `2` = R).

Face buttons are positional by default (`psp_layout 1`), so the shapes sit where a
PSP player expects them and printed cheat codes can be entered by shape:

| PSP | Switch |
|---|---|
| Triangle | X (top) |
| Square | Y (left) |
| Circle | A (right) |
| Cross | B (bottom) |

This is the default and needs no config.txt entry. Set `psp_layout 0` if you want
the Nintendo confirm/cancel convention instead (A = Cross, B = Circle). The old
name `xbox_layout` still parses and means the same thing.

config.txt only ever lists settings that differ from the defaults, so a stock file
has no `psp_layout` line and none of the sixteen `key_*` lines. Add any of them by
hand to remap a button and the line is kept; delete a line to go back to default.

| Switch | On foot | In vehicle |
|---|---|---|
| Left stick | Move | Steer |
| Right stick | Look around | Horizontal camera |
| B (Cross) | Sprint | Accelerate |
| Y (Square) | Jump | Brake / reverse |
| A (Circle) | Attack / fire weapon | Car weapon |
| X (Triangle) | Enter vehicle / skip phone call | Enter / exit vehicle |
| L | Answer phone / pickup / sub-mission | Sub-mission |
| R | Target / scope view | Hand brake |
| ZL | -- | Look left |
| ZR | Look behind | Look right |
| ZL + ZR | -- | Look behind |
| D-pad Up / Down | Cycle camera, scope zoom | Cycle camera, horn |
| D-pad Left / Right | Cycle weapon / cycle target | Cycle radio stations |
| R + D-pad Down | Toggle free aim | -- |
| L3 | Horn | Horn / toggle siren |
| R3 | -- | Recentre camera behind car |
| Plus | Pause menu | Pause menu |
| Minus | Pause menu (back action) | Pause menu (back action) |

Deviations from the PSP original, and why:

* **ZL / ZR** — the PSP has no second pair of shoulder buttons. The engine drives
  look-left/right/behind from analogue trigger fields the mobile build added, so
  ZL/ZR fill them. On the PSP this was L + the analogue nub.
* **L3 = horn** — the horn shares its `CControllerState` field with the d-pad, so
  no button id can sound it alone; a hook on `CPad::GetHorn` gives it a dedicated
  button. The d-pad horn still works. This matches the PS2 release, which puts the
  horn on L3. `THUMBL`/`THUMBR` otherwise reach no engine code at all.
* **R3 = recentre camera** — a spare id that had a reader; not a PSP behaviour.
* **Minus = pause** — the PSP's Select is Camera Modes, but
  `CPad::CycleCameraModeJustDown()` has zero callers in this build, so Select would
  do nothing. Set `key_minus SELECT` if you want it faithful-but-dead.
* **Hand brake** moves to an A + Y combo on Controller Setups 3 and 4, and the horn
  swaps between d-pad Up and Down by setup. That is the engine's own option, not
  the port's.

### Cheats

All 46 cheats are decoded straight out of the binary and every button they need is
reachable, so they can be entered on a Switch pad. Note that cheats using Up or
Down were impossible in earlier builds, because the d-pad ids were mis-mapped.

Enter during gameplay, by shape (see the table above):

| Sequence |
|---|
| Up, Square, Square, Down, Left, Square, Square, Right |
| Up, Circle, Circle, Down, Left, Circle, Circle, Right |
| Up, Cross, Cross, Down, Left, Cross, Cross, Right |
| L, R, Triangle, L, R, Circle, L, R |
| L, R, Circle, L, R, Cross, L, R |
| L, R, Cross, L, R, Square, L, R |
| L, R, Square, L, R, Triangle, L, R |
| L, L, Triangle, R, R, Cross, Square, Circle |
| L, L, Circle, R, R, Square, Triangle, Cross |
| Up, Down, Circle, Up, Down, Square, L, R |
| Up, Down, Cross, Up, Down, Triangle, L, R |
| Up, Down, Square, Up, Down, Circle, L, R |
| Up, Down, Triangle, Up, Down, Cross, L, R |
| L, L, Left, L, L, Right, Triangle, Circle |
| L, L, Left, L, L, Right, Circle, Cross |
| L, L, Left, L, L, Right, Cross, Square |
| L, L, Left, L, L, Right, Square, Triangle |
| L, L, R, L, L, R, Left, Square |
| L, L, R, L, L, R, Up, Triangle |
| R, R, L, R, R, L, Right, Circle |
| R, R, L, R, R, L, Down, Cross |
| R, Triangle, Cross, R, Square, Circle, Left, Right |
| R, Square, Circle, R, Triangle, Cross, Up, Down |
| L, Up, Left, R, Triangle, Circle, Down, Cross |
| L, Up, Right, R, Triangle, Square, Down, Cross |
| L, Down, Right, R, Cross, Square, Up, Triangle |
| L, Down, Left, R, Cross, Circle, Up, Triangle |
| Triangle, Triangle, R, Square, Square, L, Cross, Cross |
| Square, Square, R, Cross, Cross, L, Circle, Circle |
| Cross, Cross, R, Circle, Circle, L, Triangle, Triangle |
| Circle, Circle, R, Triangle, Triangle, L, Square, Square |
| Triangle, Circle, Down, Triangle, Circle, Up, L, L |
| Circle, Cross, Down, Circle, Cross, Up, L, L |
| Cross, Square, Down, Cross, Square, Up, R, R |
| Square, Triangle, Down, Square, Triangle, Up, R, R |
| Down, Down, Down, Triangle, Triangle, Circle, L, R |
| Down, Down, Down, Circle, Circle, Cross, L, R |
| Down, Down, Down, Cross, Cross, Square, R, L |
| Cross, Cross, Cross, Down, Down, Right, L, R |
| Up, Up, Up, Square, Square, Triangle, R, L |
| Up, Up, Up, Circle, Circle, Cross, L, R |
| Up, Up, Up, Cross, Cross, Square, R, L |
| Up, Up, Up, Triangle, Triangle, Circle, L, R |
| Circle, Right, Cross, Up, Right, Cross, L, Square |
| Triangle, R, L, Down, Down, R, R, Triangle |
| L, R, L, R, Up, Down, L, R |

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no direct affiliation with Take-Two Interactive Software, Inc. or Rockstar Games, Inc.
"Grand Theft Auto" and "Grand Theft Auto: Liberty City Stories" are trademarks of their respective owners.
All Rights Reserved.

No assets or program code from the original game or its Android port are included in this project.
We do not condone piracy in any way, shape or form and encourage users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is licenced under the MIT License.
Please see the accompanying LICENSE file.
