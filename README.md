<div align=center>

<img src="extras/banner.png" alt="Banner" width="50%">

</div>
<h1 align=center>GTA: Liberty City Stories - Nintendo Switch port</h1>

This is a wrapper/port of the Android version of Grand Theft Auto: Liberty City Stories (v2.4.379).
It loads the original game binary, patches it and runs it.
It's basically as if we emulate a minimalist Android environment in which we natively run the original Android binary as is.

### About this fork

This is an unofficial fork of [NaGaa95/gtalcs_nx](https://github.com/NaGaa95/gtalcs_nx)
with a set of controller fixes and, from 1.0.4, an in-game mod menu.
**All credit for the port itself goes to NaGaa95**, and to Andy Nguyen and fgsfds,
whose loader it is built on. I did not write the port — this fork changes how
controller input reaches the engine, and adds a menu on top of it.

**I am not a developer and know very little programming. None of the code here was
written by me.** All of it — the controller fixes and the whole of Liberty Menu —
was written by [Claude Code](https://claude.com/claude-code). My part was having
the ideas, saying what I wanted, running every build on hardware and reporting what
actually happened. A good deal of it was wrong the first time and got fixed because
a test on real hardware disagreed with the theory. The reasoning behind each change
is in [CHANGELOG.md](CHANGELOG.md) so anyone can check it, and the code comments
explain what was found in the game binary and why.

If you hit the same problems I did — the d-pad doing nothing, cheat codes not
working, ZR sounding the horn while looking right — this should fix them.

### Which release do I want?

| | What it is |
|---|---|
| **1.0.3+r2** | The controller fixes only. The game exactly as it shipped, playing correctly on a Switch pad. **Choose this if you do not want a mod menu.** |
| **1.0.4** | Everything in 1.0.3+r2, plus **Liberty Menu** — an in-game mod menu on the Minus button. |

Both are the same port and the same install. Nothing in 1.0.4 happens unless you
open the menu and ask for it, but if you would rather the option were not there at
all, 1.0.3+r2 is unchanged and stays available.

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
time you run the game. See **[CONFIG.md](CONFIG.md)** for the full list of `config.txt` settings, every
action name, and examples. config.txt is optional and only records settings that
differ from the defaults, so a stock file is four lines.

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
| Minus | **Open Liberty Menu** | **Open Liberty Menu** |

Deviations from the PSP original, and why:

* **ZL / ZR** — the PSP has no second pair of shoulder buttons. The engine drives
  look-left/right/behind from analogue trigger fields the mobile build added, so
  ZL/ZR fill them. On the PSP this was L + the analogue nub.
* **L3 = horn** — the horn shares its `CControllerState` field with the d-pad, so
  no button id can sound it alone; a hook on `CPad::GetHorn` gives it a dedicated
  button. The d-pad horn still works. This matches the PS2 release, which puts the
  horn on L3. `THUMBL`/`THUMBR` otherwise reach no engine code at all.
* **R3 = recentre camera** — a spare id that had a reader; not a PSP behaviour.
* **Minus opens Liberty Menu** (1.0.4). It has no game action bound to it by
  default, so nothing else fires when the menu appears. The PSP's Select is Camera
  Modes, but `CPad::CycleCameraModeJustDown()` has zero callers in this build, so
  Select would do nothing anyway. You *can* bind an action to Minus in
  `config.txt` if you want one — the menu still opens — but it will trigger at the
  same moment the menu appears, which is why the default is `NONE`.
* **Alternate control schemes exist in the engine but are unreachable.** `CPad`
  keeps a control-scheme number that `CPad::GetHandBrake` and `CPad::GetHorn` branch
  on, and on some values the hand brake becomes a Cross + Square combo instead of R.
  This build exposes no Controller Setup option in its menus, so that value never
  changes: the hand brake is R, confirmed on hardware.

## Liberty Menu

**Press Minus during play.** D-pad to move, A to choose, B to go back, Minus or B
again to close. It draws through the game's own help box, so it looks like part of
the game rather than an overlay.

It is **entirely native**: no CLEO, no scripts, nothing from anyone else's mod.
Every entry is a call into a function the game already exports, found by
disassembling the retail binary — the cheats are literally the game's own cheat
functions, and the rest is the same idea applied to things the game can do but
never offered a way to ask for.

```
Cheats                >  by category: player, wanted level, weather & time,
                         vehicles, peds
Teleport              >  every named zone, grouped by island, plus your own
                         map marker
Spawn vehicle         >  all 83 land, sea and air vehicles, by name
Player modifications  >  bodyguards, clear wanted level, invincible,
                         unlimited ammo, never tired, never wanted
Vehicle modifications >  colours, repair, flip upright, vehicle invincible,
                         vehicles fly
Misc                  >  no height limit
```

A few notes on the less obvious ones:

* **Teleport → Map marker** goes to the marker *you* placed, and tells you if
  there isn't one rather than sending you to a stale position. It takes your
  vehicle with you, and keeps your altitude if you are flying.
* **Invincible** uses the game's own immunity flag rather than topping your health
  up each frame, so a single lethal hit no longer kills you. It also keeps the car
  you are in from burning down, because an exploding car kills its occupants
  outright with no damage check that any flag can stop.
* **Unlimited ammo** puts your original ammo back when you switch it off.
* **Vehicles fly** gives whatever you are driving the game's own helicopter flight
  model. Vehicles that already fly are left alone.
* **Normal traffic** exists because the black and white traffic cheats have no
  off switch in the game — once set, they stay set for the session.
* **Spawn vehicle** names every vehicle from the game's own text, so the names are
  the game's, localised, not a hardcoded list.

Everything is off until you turn it on, and nothing is written to your save.

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
| Play as Pedestrian *(does nothing in this build)* | L, L, Left, L, L, Right, Square, Triangle |
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

The Ko-fi below belongs to **NaGaa95**, who made this port — it is not mine, and I
am not asking for anything. The port is the hard part and it is his; this fork only
fixes the controls and adds a menu on top. If you would like to support the work,
support him.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no direct affiliation with Take-Two Interactive Software, Inc. or Rockstar Games, Inc.
"Grand Theft Auto" and "Grand Theft Auto: Liberty City Stories" are trademarks of their respective owners.
All Rights Reserved.

No assets or program code from the original game or its Android port are included in this project.
We do not condone piracy in any way, shape or form and encourage users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is licenced under the MIT License.
Please see the accompanying LICENSE file.
