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
| ped position | `ped+64` (x,y) / `ped+72` (z) | `CPed::Teleport` `str d0,[x20,#64]` |
| radar blips | `CRadar::ms_RadarTrace`, 75 × 60 | `CRadar::SetTargetBlip` |
| blip type / pos | `+40` (2=char, 4=coord) / `+12,+16,+20` | ditto + runtime dump |

`FindPlayerPed()` returns the player ped.

**`CVector` is passed by pointer, not in `s0/s1/s2` — do not trust the mangled
name.** `CPed::Teleport(CVector)` reads `ldr d0,[x1]` / `ldr s2,[x1,#8]`, so the
argument arrives as an address in `x1` even though the symbol says by-value and a
three-float aggregate would normally be an HFA. Declaring it flattened made every
teleport dereference garbage, and cost a long detour hunting the map marker in
`ms_RadarTrace` — the blip was never the bug. `CPopulation::AddPed`'s
`RK7CVector` is a pointer too. **Check the disassembly for `ldr` from `x1`
before assuming any `CVector` argument is in the float registers.**

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
- **`TankCheat` is not a tank cheat.** It walks a counter over the vehicle model
  range (130..216, skipping planes) and hands the result to `VehicleCheat`, so it
  spawns a *different* vehicle each press. It is listed as "Random vehicle".
- **`VehicleCheat(modelId)` is only safe for cars and bikes.** It picks the class
  from the id alone (`CBike` for `0xCA..0xD2`, `CAutomobile` for everything else),
  so a boat is constructed as a car and the game dies — that is what crashed on
  the Reefer and the Speeder. It also places the vehicle on the nearest path node
  within 100 units, which puts cars out of view. `menu.c` does the spawn itself:
  `RequestModel` + `LoadAllRequestedModels`, `CVehicle::operator new`, the right
  constructor, matrix, `CWorld::Add`. **Choose the class with
  `CModelInfo::IsCarModel` / `IsBikeModel` / `IsBoatModel`, never an id range.**
- **Entity layout** (from `VehicleCheat`'s own stores): matrix at `+16`, three
  16-byte rotation rows at `+16`/`+32`/`+48`, position at `+64` (x,y) and `+72`
  (z), status at `+784`. Row 1 (`+32`) is the forward vector — `SetHeading`
  builds row0 `(cos,sin,0)`, row1 `(-sin,cos,0)`, and GTA faces `+y`. Copying a
  ped's 48 rotation bytes onto a vehicle is an easy way to face it correctly.
- **`CVehicle::operator new` sizes**: `0x7b0` `CAutomobile`, `0x6a0` `CBike`,
  `0x610` `CBoat`, `0x490` `CHeli`, `0x420` `CTrain`. Read them off the callers of
  each constructor. **`CPlane` has no callers at all** in this build — LCS has no
  flyable plane — so there is no size to read and planes are not spawnable.
- **Teleport to a path node, not to a zone centre.** `FindGroundZForCoord`
  returns the highest surface at an x/y, so a zone whose middle is a building
  drops the player on its roof. `gpThePaths` + `CPathFind::FindNodeClosestToCoors`
  gives a road instead; node records are 20 bytes with x/y/z as `int16` at
  `+4`/`+6`/`+8`, each an eighth of a world unit.
- **Vehicles name themselves.** `CCurrentVehicle::Display` — the bottom-right
  readout when you get in — reads a GXT key stored **inline at model info
  `+0x52`** and passes it to `CText::Get`. So `ms_modelInfoPtrs[id] + 0x52` plus
  `CText::Get` names every vehicle, localised, with nothing hardcoded.
- **Use `CText::Get`, not `CText::Exists`.** `Exists` returned false for every
  key tried, including `CHEAT1`, which the game itself resolves through `Get`.
  Both call `CKeyArray::Search`; whatever `Exists` gates on is not what `Get`
  needs. `CText::msInstance` is the object, reached without `TheText()`.
- **Zone boxes come from `gpTheZones`.** `FindZoneByLabelAndReturnIndex` plus
  `GetNavigationZone` (the named zones are navigation zones, type 0); `CZone` is
  72 bytes with its box at `+8`/`+20` and its name via `GetTranslatedName`.
  Coordinates scraped from the CLEO script matched these exactly — the script
  read was right all along and only the **English names guessed against it** were
  wrong, which is what sent "Atlantic Quays" to Portland Harbor.
- **Names are gone, but ids are enumerable.** `CBaseModelInfo::SetModelName`
  stores only a CRC-32 of the uppercased name (`CKeyGen::GetUppercaseKey`,
  standard reflected table at `0x24532c`, no final complement) and `strcpy`s the
  string **only when chunk files are off**, which they are not — so no name
  survives, in the binary or the wads, and a name can only be tested by asking
  the running game via `GetModelInfo(name, &id)`. Do not go looking for a name
  table; there isn't one. **What you can do is walk ids `0..msNumModelInfos` and
  classify each with the `Is*Model` predicates** — that gives a complete vehicle
  list with no names involved, which is how the spawn menu is built.
- **Zone names come from the game.** `TheText()` + `CText::Exists` /
  `CText::GetUTF8(key, buf, len)` resolve GXT keys, so names arrive localised.
  `CText` is not loaded at `patch_game` time — look them up lazily.
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

- **Zone names never resolve.** `0/29` GXT keys hit, *and* a probe with `CHEAT1`
  -- a key `VehicleCheat` itself passes to `CText::Get` and which visibly works
  in game -- also missed. So the keys are not the problem; the lookup is. First
  suspect was `TheText()`: it is a weak symbol *and* a lazy constructor that
  installs a fresh empty `CText` when the slot is null, which would answer every
  key with a miss. Now read `CText::msInstance` directly (`TheText()`'s GOT entry
  is relocated to it) and log the pointer next to the probe.
- **Bodyguards** — the ped model is now streamed before `AddPed`, but the crash
  itself is unconfirmed as fixed; it has not been retested.
- **Models 211–216 are helicopters typed as cars.** Screenshots confirm a Hunter,
  three Mavericks and two small unmarked helis, but their model info says vehicle
  type 0, so they are built as `CAutomobile` and cannot fly. **198 and 199 are
  the two the game types as helicopters, and those do fly.** Forcing `CHeli` on
  a car-typed model is untried and could well crash.
- **Bodyguard weapon** — `BODYGUARD_WEAPON` is a guessed `eWeaponType` (17). The
  spawn logs the number it used; adjust once it is clear what they are holding.
  Ped type is `PEDTYPE_GANG1` (7) with `CPopulation::ChooseGangOccupation(0)`
  picking the model, and `CPed::SetPlayerToFollow(0)` doing the following.
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
