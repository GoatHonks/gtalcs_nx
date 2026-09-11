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
currently **on**; turn it off for a release build. Every line is `fflush`ed to
the SD card and the game's own `printf` comes through the same function, so the
log costs real boot time — measure with it off before blaming anything else.
Lines are stamped `[seconds.millis]` from the first line, which is what makes
timing questions answerable at all.

`data_main.wad` is **not a zip**: no `PK` magic, no end-of-central-directory. The
engine reads it by sector (`STREAM[n] sec=56664 nsec=495`, and the bytes
returned are exactly `nsec * 2048`), so there is nothing to unpack and nothing
addressable if you did. Contents are high entropy, so any decompression happens
inside the engine after the read, not in a container layer we could remove.

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
- **`CHud::m_HelpMessage` is 0x200 bytes — 255 characters plus a terminator.**
  Push more and the game keeps a truncated copy, the clobber check never matches
  again, and you get the machine-gun above with nothing on screen. Adding a
  sixth toggle was enough (246 → 268 characters). Both lists are windowed to
  `MENU_ROWS` and the render stops at `HUD_HELP_CHARS - 40`, so **check the
  rendered length, not the row count, when adding entries.**

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
| map marker | `GRadarMap+128` (x,y), **`+120` byte = is-set** | `RadarMap::Update` toggle; with/without diff |
| vehicle handling | `vehicle+392` | `CBoat::DisplayHandlingData` `ldr x0,[x0,#392]` |
| top speed | `tHandlingData+136` | `ConvertDataToGameUnits` `fmul` by 1/180 |
| gearbox | `tHandlingData+52`, reads its own `+84` | `cTransmission::InitGearRatios` |
| `Teleport` (virtual) | **`vtable+0x78` for every entity class** | `_ZTV4CPed`/`11CAutomobile`/`5CBike`/`5CBoat` relocs |

**A data symbol is not dead just because you grepped for it.** "Fly higher"
stored two floats in `VehicleNames`, on the strength of a grep for `adrp` +
`add #0x1c8` finding no readers. That only covers **same-translation-unit**
access; everything else reaches a data symbol through the **GOT**.
`objdump -R | grep VehicleNames` shows a `GLOB_DAT` relocation and five readers,
and the symbol sits between `HandlingFilename` and `BOAT_BUOYANCY_DAMPING` in
the vehicle handling data. Writing there cost **every vehicle in the world its
collision** — spawned cars fell through the map, the streets emptied of traffic —
and sent me hunting a spawn bug that did not exist, for several rounds, while
the user was telling me it began when that feature landed. **Check `objdump -R`
for a GOT entry before believing any data symbol is unused.**

**Do not dereference a resolved address inside `menu_init`.** It runs from
`patch_game`, and `so_try_find_addr_rx` returns a `load_virtbase` address that
**is not mapped until `so_finalize`** — reading through one there is an instant
boot crash with the log stopping right after the port's last hook line. Storing
the address is fine, and is why every other symbol got away with it for months.
Anything that needs to *read* game memory belongs on the lazy path that runs when
the menu is first opened.

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
- **Spawn with `VehicleCheat(modelId)`** — the path `TrashmasterCheat` uses.
  It **crashes on boats** (builds them as `CAutomobile`), so those go through
  `SpawnInModel`; it places the vehicle on a path node up to 100 units away
  rather than next to you; and **it gives up silently when there is no path node
  within 100 units**, which is why spawning away from a road produced a toast and
  no vehicle. `SpawnInModel` is the fallback for that case — detected by the pool
  scan finding nothing new. Both costs are worth paying: a long detour spent
  hand-rolling a spawner and then swapping in `SpawnInModel` achieved nothing,
  because the vehicles were falling for a reason that had nothing to do with how
  they were built (see the `VehicleNames` entry).
- **The vehicle pool**, from `CCarCtrl::RemoveDistantCars`: entries at `+0`, flag
  bytes at `+8`, count at `+16`, stride 1968, negative flag means free. Snapshot
  the taken slots before a spawn to find the vehicle afterwards — neither
  spawner returns one.
- **`SpawnInModel(int model, CVector &pos)`** Building a vehicle by hand (operator new, constructor, matrix,
  `CWorld::Add`) produced one that **fell straight through the map**: x and y
  frozen, z 11.7 → 10.5 → 4.6 → −6.4 → −43.5 until `CWorld::RemoveFallenCars`
  swept it below −100, about 3.5 seconds later. Boats came out with `z = NaN`.
  `SpawnInModel` differs in a dozen ways — `RequestModel(id, **4**)`, `createdBy`
  **2**, class by `Is*Model` with `CHeli::ActivateHeli(false)` and a bike flag at
  `+1540`, `z = FindGroundZForCoord + CEntity::GetDistanceFromCentreOfMassToBaseOfModel`,
  `CCarCtrl::JoinCarWithRoadSystem`, flag writes at `+717`, and the level byte at
  **`+390`**, not the `+126` that `SetupBigBuilding` uses. The `CVector` is by
  reference and is written back with the settled position.
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
- **`CTheZones` methods take `gpTheZones` as `this`.** `GetLevelFromPosition` is
  `(CTheZones*, const CVector*)`, not a free function — calling it with the
  vector in `x0` returned nonsense levels and then faulted. Check `x0` at a call
  site before assuming a `CTheZones::` symbol is static.
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

- **Bodyguards** — crashed four times, all on the same call. The instrumented
  log settled it: `AddPed` **succeeds**, and `CPed::SetPlayerToFollow` then dies
  on `ldr x10,[x19,#1552]` / `ldrsh w8,[x10,#124]` with no null check — a ped
  fresh from `AddPed` has nothing at `+1552`. Use `CPed::SetLeader`, which is
  one null-checked store and is what `PlaceGangMembersInFormation` uses.
  **Read the callee's first few instructions before handing it a new object.**
  Fixed and confirmed working. Three earlier guesses (streaming, model residency,
  the clump check) were all aimed at the wrong call; the staged logging that
  finally found it should have been the first move.
- **Models 211–216 are helicopters typed as cars — and they fly anyway.** Their
  model info says vehicle type 0, so the menu builds them as `CAutomobile`, and
  they still fly when entered. Flight comes from the handling data, not the
  vehicle type, so *do not* assume the type field gates behaviour — it only picks
  the class. 211, 212 and 216 have no textures in this build.
  **198 and 199, the two the game does type as helicopters, are the useless
  ones**: 199 takes off by itself the moment it exists, and **198 crashes the
  game on spawn** and is excluded from the list. Both go through the identical
  `CHeli` path, so the class is not at fault — 198 is an incomplete model. The
  suspected mechanism is `CHeli`'s `FillNodeArray` on the clump at `+112`, which
  walks the rotor hierarchy and would take a null from a model with no geometry;
  that is a theory, not a finding.
- **Bodyguard weapon** — `BODYGUARD_WEAPON` is a guessed `eWeaponType` (17). The
  spawn logs the number it used; adjust once it is clear what they are holding.
  Ped type is `PEDTYPE_GANG1` (7) with `CPopulation::ChooseGangOccupation(0)`
  picking the model, and `CPed::SetPlayerToFollow(0)` doing the following.
- **Strong tyres** — candidate flags are `CVehicle::bCheat3`…`bCheat10`, unnamed.
- **Unused vehicles (corpse, mini, topfun, …) are not in the stock game.** The
  mod in `MODINFO/gta lcs unused vehicles` adds them as models 218/223/224/226/239
  by shipping a modified `DEFAULT.IDE` plus textures. Nothing to do in the menu:
  the spawn list is built by **walking the model table**, so installing that mod
  makes them appear on their own. That IDE is also the authority on names —
  it is what says 211 is `rcgoblin` and 212 `rcraider`, two different RC
  vehicles rather than the duplicate I had assumed.
- **Make the Dodo fly** — asked for, not started. It is model 164, typed as a
  car, and `MODINFO/dodo fly.csa` is **840 bytes of script**: a whole flight
  model that reads the pad, does vector maths on the vehicle matrix and applies
  forces every frame. Porting it is writing a flight controller in C, not
  flipping a flag — a real feature, not a quick one.
- **Vehicle editor** — done: colours, top speed, flip upright. The
  handling data pointer is at `vehicle+400` (`CHeli`'s constructor writes it from
  an array indexed by model info `+102`, stride `0xe0`), and `cHandlingDataMgr`
  exposes `GetFlyingPointer` / `GetBoatPointer` / `ModifyHandlingValue`. Note
  handling records are **shared per model**, so editing one changes every vehicle
  of that type.

## Working style that has paid off

Change one thing, build, test on hardware, read `debug.log`. Nearly every bug in
this project was found by adding a log line and looking, not by reasoning: the
off-by-one in menu selection, the marker blip type, the latched analogue sticks,
the tip overwriting the menu. When a theory and a log disagree, the log is right.

## Top speed: tried, removed

`fMaxVelocity` is `tHandlingData+136` (via `vehicle+392`), identified by the 1/180
km/h conversion in `ConvertDataToGameUnits`, and `cTransmission` sits inline at
`handling+52` reading that same word at its own `+84`, so `InitGearRatios` has to
be re-run after writing it. All of that worked. It was **removed on request**:
handling records are shared per model, so it changed every vehicle of that type
including the AI's, and that is not a per-car edit however it is presented. The
offsets are recorded here in case it is ever wanted again; the feature is not.

## Teleport moves whatever the player is

A ped riding in a car takes its position from the vehicle every frame, so
teleporting the ped while driving moved nothing. `menu_teleport_xy` now calls
`Teleport` on the **vehicle** when `FindPlayerVehicle()` returns one.

`Teleport` is virtual and lands in the **same vtable slot for every entity
class** — `R_AARCH64_ABS64` relocations at `_ZTV4CPed+0x78`,
`_ZTV11CAutomobile+0x78`, `_ZTV5CBike+0x78`, `_ZTV5CBoat+0x78`. So one indirect
call handles ped, car, bike, boat and `CHeli` without a class switch.

**But `_ZTV...` is the address of the vtable object, not of its first function
pointer.** `+0` is offset-to-top, `+8` is RTTI, entries start at `+0x10`
(`_ZTI11CAutomobile` sits at `_ZTV11CAutomobile+8`, and slot 0, `CPhysical::Add`,
at `+0x10`). The pointer an object stores is `symbol + 0x10`. So a relocation at
`symbol+0x78` is index **13**, not 15. Indexing by 15 called `Render()`, two slots
on: every teleport logged its destination, returned cleanly, and moved nobody —
no crash, no error, just nothing. **Subtract the 0x10 header before dividing.** The `CVector` is by
pointer here too. Zero the velocity at `+144` afterwards or the car keeps the
speed it had when the menu opened.

## The spawn watchdog is time-limited

It logged a parked, perfectly healthy car twice a second for over 100 seconds and
buried everything else in the log. `VEH_TRACK_MS` stops it at 12 s — a spawn that
is going to fall does so in about 3.5.

**`CEntity::Teleport` is a no-op — one `ret`, no body.** It is what `_ZTV5CHeli`,
`_ZTV6CPlane`, `_ZTV6CTrain` and `_ZTV8CVehicle` all hold in slot `+0x78`; only
`CPed`, `CAutomobile`, `CBike` and `CBoat` override it. So the virtual call
silently does nothing for a real helicopter (198/199) and would have reported
success. Compare the slot against the resolved `CEntity::Teleport` and, when it
matches, do the move by hand: `CWorld::Remove`, write `+64`/`+72`, `CWorld::Add` —
CAutomobile's first and last steps. **Do not call `CAutomobile::Teleport` on a
`CHeli` instead**: `CHeli` allocates `0x490` bytes and that function stores to
`+1568` and `+1600`.

Note 211–216 are built as `CAutomobile` (their model type says car), so they take
the real override; it is only 198/199 that need the fallback.

Teleporting a flying vehicle onto the ground is a crash, so when it is more than
`AIRBORNE_MIN` (8 units) clear of the ground it keeps its altitude above the
destination instead.

## Repairing a vehicle

`CVehicle::ExtinguishCarFire` is the whole feature in one exported call, and
reading it hands over the offsets: health `+736` (raised to 300 by `fmaxnm`), the
`CFire *` at `+696`, `CDamageManager` at `+0x3b0`, and the **burn timer at
`+1804`**, which is the one that matters — a rolled car arrives already alight
with that counter running and explodes moments after being righted. Write health
to 1000 afterwards; the call only lifts it to 300.

`Fix` is **not virtual**: `CAutomobile::Fix` and `CBike::Fix` exist only as PLT
entries, in no vtable, so it has to be chosen by class. The Teleport slot does
that for free — whichever override is sitting in it names the class, with no
`_ZTV` symbols to resolve. "Flip upright" repairs as part of the same action.

## The map marker, and the diff that has to straddle the transition

`GRadarMap+128` is the position, **`+120` (a byte) is whether a marker exists.**
`RadarMap::Update` toggles it in two places:

```
ldrb w8, [x19, #120] / eor w9, w8, #0x1 / strb w9, [x19, #120]
cbnz w8, <skip>      ... then compute and store the position at +128
```

Confirming on the map flips it, so placing sets it and pressing again clears it.
The position is written only on the 0 → 1 edge, which is why a cleared marker
leaves the old coordinates intact at `+128` — and `+128` is the map *cursor*
anyway, written from `OS_PointerGetNumber` → `TransformScreenSpaceToRadarPoint` →
`TransformRadarPointToRealWorldSpace`.

**I called `+136` the flag first, and it was wrong in an instructive way.** It fit
both samples I had — 0 without a marker, 1 with one — but the "without" sample was
taken *before a marker had ever been placed that session*, not after one was
removed. `+136` never returns to 0 once set, so every cleared marker still
teleported. **A before/after diff has to straddle the actual transition**; two
states that merely differ are not a diff of the thing you are asking about.

Ruled out for good, by resolving every GOT reference rather than grepping:
`CMenuManager::m_TargetIsOn`, `m_fTargetPos`, `m_TargetBlipIndex` and
`CRadar::MapWayPoint` are read and written **only** by `SetSaveData` and
`LoadSaveData`, and `CRadar::SetTargetBlip` has exactly one caller in the binary
(`LoadSaveData`). They are save-game fields. The marker is also **not a blip**:
with one placed, `ms_RadarTrace` holds only the player (sprite 40) and the home
icon (19).
