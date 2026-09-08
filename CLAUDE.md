# Liberty Menu — GTA: Liberty City Stories (Switch port)

An in-game mod menu built on top of [NaGaa95/gtalcs_nx](https://github.com/NaGaa95/gtalcs_nx),
via the [GoatHonks fork](https://github.com/GoatHonks/gtalcs_nx) (controller fixes).

The port is a **wrapper**, not a decompilation: it loads the retail Android
`arm64-v8a` `libGame.so`, patches it and runs it inside a minimal fake-Android
environment. Everything the menu does is a call into that library.

## The one fact that makes this project tractable

`libGame.so` exports **18,181 defined symbols** with full mangled C++ names.
Every game function and global the menu touches is resolved **by name** at
runtime through `so_try_find_addr_rx()` — no hardcoded offsets, no signature
scanning. When you need something, look for the symbol first:

```bash
# dump the symbol table once, then grep it
aarch64-none-elf-nm -D PORT/gtalcs/libGame.so
```

Addresses quoted in comments are for *this exact build* (2.4.379 arm64) and are
there to show where a fact came from, never to be used directly.

## Layout

| Path | What |
|---|---|
| `source/menu.c` | Liberty Menu. All features live here. |
| `source/menu.h` | `menu_init()` / `menu_tick()` |
| `source/main.c` | port entry, main loop, input; publishes `g_menu_pad_down` / `g_menu_open` |
| `source/hooks/game.c` | port's own game hooks; calls `menu_init()` from `patch_game()` |
| `source/config.c` | `config.txt` (self-documenting; the writer emits its own comments) |
| `PORT/` | local copy of the SD card. **gitignored** — 2GB of Rockstar's data |
| `MOD/` | the CLEO mod pack this was originally built against. **gitignored**, unused |

## Building

Must run from the devkitPro msys2 shell — git bash picks the wrong `sed` and
cannot find `bin2s`:

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc "cd <repo> && make"
```

Output is `gtalcs_nx.nro` (the name is pinned in the Makefile; leaving it as
`$(notdir $(CURDIR))` makes the output follow the directory name, and that name
is baked into the NRO's module field, not just the filename).

Required portlibs (note it is `switch-openal-soft`, not `switch-openal`):

```bash
pacman -S --needed switch-openal-soft switch-mpg123 switch-ffmpeg switch-dav1d switch-zlib switch-bzip2
```

`DEBUG_LOG` in `source/config.h` controls `/switch/gtalcs/debug.log`. It is
currently **on**; turn it off for a release build.

## How the menu works

**No hooks.** `menu_tick()` is called from the main loop in `main.c` right after
`update_gamepad()`, which runs on the same thread as the game frame, so it can
call game functions directly. It is gated on the port's app state `9` (in-game):
the help box it draws through belongs to the in-game HUD.

**Drawing** goes through `CHud::SetHelpMessage` — the game's own help box, which
makes the menu look native. Two hard-won rules:

- The help box is a **single shared slot** the game also writes tips into, so the
  menu re-pushes its text when `CHud::m_HelpMessage` no longer matches what we
  last wrote. Both the top level and the cheats list need this check.
- **Never** re-push unconditionally every frame: `SetHelpMessage` restarts the
  box's animation and replays its sound, so at 60Hz the cue machine-guns and the
  text never finishes appearing.

**Input**: `main.c` publishes the pad state and, while `g_menu_open` is set,
sends the game no buttons and **centred sticks**. The stick part matters — an
early version returned before the axis code and left the sticks latched, so the
player kept walking on his own.

**Minus is reserved.** `config.c` forces `key_minus = GPAD_BUTTON_NONE` after
parsing, so no config can rebind it; any other binding would fire a game action
at the same moment the menu opened.

## Verified field offsets and entry points

All derived by disassembling the named function in this build.

| What | Where | Found via |
|---|---|---|
| player health | `ped+1440` (float) | `HealthCheat` `str s8,[x0,#1440]` |
| player armour | `ped+1444` (float) | `ArmourCheat` `str s8,[x0,#1444]` |
| sprint energy / max | `ped+3116` / `ped+3120` | `CPlayerPed::RestoreSprintEnergy` |
| `CWanted` | `ped+0x928` | `WantedLevelDownCheat` → `CheatWantedLevel(0)` |
| weapon array | `ped+1716`, stride 28 | `CPed::SetAmmo` |
| radar blips | `CRadar::ms_RadarTrace`, 75 × 60 | `CRadar::SetTargetBlip` |
| blip type / pos | `+40` (2=char, 4=coord) / `+12,+16,+20` | ditto + runtime dump |

`FindPlayerPed()` returns the player ped. `CPed::Teleport(CVector)` takes three
floats: `CVector` is a homogeneous float aggregate, so AAPCS64 passes it in
`s0/s1/s2` — declare it flattened, not as a struct.

`HealthCheat` is the only cheat taking a parameter, and that parameter gates the
game's **own on-screen message** — pass 1 for the native confirmation, 0 to top
up silently.

## Things established the hard way

- **`CPed::SetAmmo` validates the slot itself** (`GetWeaponInfo`, early-out on
  −1), so refilling by weapon *type* is safe; poking the weapon array directly
  means guessing bounds inside the ped struct.
- **Dead cheats exist.** `WallClimbingCheat` etc. flip flags nothing in this
  build reads. `CMenuManager::m_PrefsInvincibility` has **zero readers**.
  Check for readers before exposing something as a feature.
- **The Rhino does exist in LCS** — `TankCheat` works.
- Mangled-name length prefixes are easy to miscount (`_Z12WeaponCheat1v`, not
  `_Z11...`). Copy them from the symbol dump rather than typing them.

## History: CLEO, and why it was removed

The menu began as a full CLEO runtime — a reimplementation of
`CRunningScript::ProcessOneCommand`, `ScriptSpace` relocation, and CLEO's own
opcodes — and it did run the real `.csa`/`.csi` scripts. It was deleted
deliberately, not because it failed:

- Every feature worth having is a direct call to an exported function anyway.
  The cheats are literally `TankCheat()`, `MoneyCheat()`.
- The interpreter carried the risk (IP desyncs, script-list corruption, opcode
  semantics inferred from a 32-bit ARM `libcleo.so`) for no benefit.
- Shipping someone else's scripts is not something this project wants to do.

`git log` has the full implementation if it is ever needed again. Notes worth
keeping from it: CLEO labels are **script-relative and negative**; `0x0DF4`
returns **−1** while a menu is open and **−2** when cancelled; and stopping a
script from inside its own execution corrupts the active list — point its `IP`
at an `END_THREAD` stub instead and let the game retire it.

## Outstanding

- **Vehicle spawner** — `CStreaming::RequestModel` + `LoadAllRequestedModels`,
  then construct/place the vehicle. `TrashmasterCheat` is a working template.
- **Bodyguards** — `CPopulation::AddPed(ePedType, model, CVector, int, bool)`
  spawns them; arming and follow behaviour need the ped task API.
- **Strong tyres** — candidate flags are `CVehicle::bCheat3`…`bCheat10`, unnamed.
- **Flying altitude limit** — *recommended dropped*. In the ARM32 build it is a
  float `80.0` inside `CVehicle::FlyingControl`; our arm64 build has no
  equivalent literal (the whole function yields only `-0.0143`, `0.62`, `1.0`),
  so it is likely read from a parameter struct. Three static approaches failed.

## Working style that has paid off

Change one thing, build, test on hardware, read `debug.log`. Nearly every bug in
this project was found by adding a log line and looking, not by reasoning: the
off-by-one in menu selection, the marker blip type, the latched analogue sticks,
the tip overwriting the menu. When a theory and a log disagree, the log is right.
