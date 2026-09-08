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
entirely with [Claude Code](https://claude.com/claude-code). My part was describing
the problems I ran into while playing and testing the results on hardware. The
reasoning behind each change is in [CHANGELOG.md](CHANGELOG.md) so anyone can check
it, and the code comments explain what was found in the game binary and why.

If you hit the same problems I did — the d-pad doing nothing, cheat codes not
working, ZR sounding the horn while looking right — this should fix them.

**This fork contains no game files.** You still need your own legally obtained
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
time you run the game. It is written out with a comment above every setting, so the
file documents itself: every option, every action name and what each one does is
in there, with no separate reference to look up.

config.txt is rewritten on every launch: your values are kept, but any comments you
add yourself are not.

## Controls

The PSP layout, checked against the engine rather than guessed. `CPad::DoCheats()`
feeds one character to `CPad::AddToCheatString()` per button pressed — T for
Triangle, C for Circle, S for Square, X for Cross, U/D/L/R for the d-pad, 1 for L
and 2 for R — so the game itself names which button each controller field belongs
to. The mapping below follows those names.

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

| Switch | On foot | In vehicle |
|---|---|---|
| Left stick | Move | Steer |
| Right stick | Look around | Horizontal camera |
| A (Circle) | Attack / fire weapon | Car weapon |
| B (Cross) | Sprint | Accelerate |
| X (Triangle) | Enter vehicle / skip phone call | Enter / exit vehicle |
| Y (Square) | Jump | Brake / reverse |
| L | Answer phone / pickup / sub-mission | Sub-mission |
| R | Target / scope view | Hand brake |
| ZL | -- | Look left |
| ZR | Look behind | Look right |
| ZL + ZR | -- | Look behind |
| D-pad Up / Down | Cycle camera, scope zoom | Cycle camera, horn |
| D-pad Left / Right | Cycle weapon / cycle target | Cycle radio stations |
| R + D-pad Down | Toggle free aim | -- |
| L3 | -- | Horn / toggle siren |
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
* **Alternate control schemes exist in the engine but are unreachable.** `CPad`
  keeps a control-scheme number that `CPad::GetHandBrake` and `CPad::GetHorn` branch
  on, and on some values the hand brake becomes a Cross + Square combo instead of R.
  This build exposes no Controller Setup option in its menus, so that value never
  changes: the hand brake is R, confirmed on hardware.

### Cheats

The 46 cheat sequences below are decoded out of the game binary itself, and the
effects are matched against the [GTA Wiki cheat list](https://gta.fandom.com/wiki/Cheats_in_GTA_Liberty_City_Stories).
Every button they need is reachable on a Switch pad — cheats containing Up or Down
could not be entered at all in earlier builds, because the d-pad ids were mis-mapped.

Enter them during normal gameplay, by shape (see the controls table above).

Three sequences are recognised by the game but do nothing: their handler clears the
input buffer and returns without calling anything, so they appear to be leftovers
disabled in this build. They are listed for completeness.

| Effect | Sequence |
|---|---|
| Aggressive Drivers | Square, Square, R, Cross, Cross, L, Circle, Circle |
| All Green Lights | Triangle, Triangle, R, Square, Square, L, Cross, Cross |
| All peds have big heads | Down, Down, Down, Circle, Circle, Cross, L, R |
| All Vehicles Chrome Plated | Triangle, R, L, Down, Down, R, R, Triangle |
| Armor | L, R, Circle, L, R, Cross, L, R |
| Black Traffic | Circle, Circle, R, Triangle, Triangle, L, Square, Square |
| Cars Drive On Water | Circle, Cross, Down, Circle, Cross, Up, L, L |
| Certain peds follow you | Down, Down, Down, Triangle, Triangle, Circle, L, R |
| Change Bike Tire Size | Circle, Right, Cross, Up, Right, Cross, L, Square |
| Clear Weather | Up, Down, Circle, Up, Down, Square, L, R |
| Commit Suicide | L, Down, Left, R, Cross, Circle, Up, Triangle |
| Destroy All Cars | L, L, Left, L, L, Right, Cross, Square |
| Display Game Credits | L, R, L, R, Up, Down, L, R |
| Faster Clock | L, L, Left, L, L, Right, Circle, Cross |
| Faster Gameplay | R, R, L, R, R, L, Down, Cross |
| Foggy Weather | Up, Down, Triangle, Up, Down, Cross, L, R |
| Get $250,000 | L, R, Triangle, L, R, Circle, L, R |
| Health | L, R, Cross, L, R, Square, L, R |
| Media Attention Meter | L, Up, Right, R, Triangle, Square, Down, Cross |
| Nearest ped enters your car | Cross, Square, Down, Cross, Square, Up, R, R |
| Never Wanted | L, L, Triangle, R, R, Cross, Square, Circle |
| Overcast Weather | Up, Down, Cross, Up, Down, Triangle, L, R |
| Peds Attack You | L, L, R, L, L, R, Up, Triangle |
| Peds Have Weapons | R, R, L, R, R, L, Right, Circle |
| Peds Riot | L, L, R, L, L, R, Left, Square |
| Perfect Traction (Down = car hop) | L, Up, Left, R, Triangle, Circle, Down, Cross |
| Play as Pedestrian | L, L, Left, L, L, Right, Square, Triangle |
| Rainy Weather | Up, Down, Square, Up, Down, Circle, L, R |
| Raise Wanted Level (by 2 stars) | L, R, Square, L, R, Triangle, L, R |
| Slower Gameplay | R, Triangle, Cross, R, Square, Circle, Left, Right |
| Spawn Rhino | L, L, Left, L, L, Right, Triangle, Circle |
| Spawn Trashmaster | Triangle, Circle, Down, Triangle, Circle, Up, L, L |
| Sunny Weather | L, L, Circle, R, R, Square, Triangle, Cross |
| Unlock All Multiplayer Stuff | Up, Up, Up, Triangle, Triangle, Circle, L, R |
| Unlock Multiplayer Stuff 1 | Up, Up, Up, Square, Square, Triangle, R, L |
| Unlock Multiplayer Stuff 2 | Up, Up, Up, Circle, Circle, Cross, L, R |
| Unlock Multiplayer Stuff 3 | Up, Up, Up, Cross, Cross, Square, R, L |
| Upside Down | Down, Down, Down, Cross, Cross, Square, R, L |
| Upside Down (alternate) | Cross, Cross, Cross, Down, Down, Right, L, R |
| Weapon Set 1 | Up, Square, Square, Down, Left, Square, Square, Right |
| Weapon Set 2 | Up, Circle, Circle, Down, Left, Circle, Circle, Right |
| Weapon Set 3 | Up, Cross, Cross, Down, Left, Cross, Cross, Right |
| White Traffic | Cross, Cross, R, Circle, Circle, L, Triangle, Triangle |
| *(recognised, but does nothing in this build)* | R, Square, Circle, R, Triangle, Cross, Up, Down |
| *(recognised, but does nothing in this build)* | L, Down, Right, R, Cross, Square, Up, Triangle |
| *(recognised, but does nothing in this build)* | Square, Triangle, Down, Square, Triangle, Up, R, R |

### Support the developer

The Ko-fi below belongs to **NaGaa95**, who made this port — it is not mine. I only
changed the controls, so if you would like to support the work, support him.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no direct affiliation with Take-Two Interactive Software, Inc. or Rockstar Games, Inc.
"Grand Theft Auto" and "Grand Theft Auto: Liberty City Stories" are trademarks of their respective owners.
All Rights Reserved.

No assets or program code from the original game or its Android port are included in this project.
We do not condone piracy in any way, shape or form and encourage users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is licenced under the MIT License.
Please see the accompanying LICENSE file.
