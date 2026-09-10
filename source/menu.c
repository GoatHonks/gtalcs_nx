/* menu.c -- Liberty Menu, an in-game menu for the LCS Switch port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Everything here is native: each entry calls the game's own functions, which
 * libGame.so exports by name (it ships 18k+ mangled C++ symbols), so there is no
 * script interpreter, no bytecode and no third-party content involved.
 *
 * An earlier version of this file ran CLEO scripts by reimplementing the script
 * VM's opcode dispatcher. That worked, but every feature worth having turned out
 * to be a direct call to an exported function anyway -- the cheats are literally
 * TankCheat(), MoneyCheat() and friends -- so the interpreter was carrying a lot
 * of risk (IP desyncs, script-list corruption, opcode semantics guessed from a
 * 32-bit ARM library) for no benefit. Calling the functions is simpler, cannot
 * desync, and keeps the menu entirely our own work.
 *
 * ---- how it draws ----
 *
 * Through CHud::SetHelpMessage, the game's own help box. That was originally a
 * workaround -- every per-frame function in the HUD render path is either large
 * or a dead stub with no callers, and hook_arm64() builds no trampoline, so
 * there was nothing to hook -- but it turned out to look like part of the game,
 * which is worth more than a custom overlay would be.
 *
 * The help box is a single shared slot the game also writes its own tips into,
 * so the menu re-pushes its text whenever something else has overwritten it.
 * Re-pushing unconditionally every frame does not work: SetHelpMessage restarts
 * the box's animation and replays its sound, so at 60Hz the cue machine-guns and
 * the text never finishes appearing.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "menu.h"

extern so_module game_mod;   // defined in main.c

// Published by main.c's update_gamepad. While the menu is open the game receives
// no buttons and centred sticks, so navigating cannot shoot, steer or punch.
extern volatile u64 g_menu_pad_down;
extern volatile int g_menu_open;

// ---- game entry points ----
typedef void (*set_help_msg_fn)(uint16_t *text, char a, char b);
static set_help_msg_fn hud_set_help_message = NULL;
static uint8_t *hud_help_forever = NULL;    // CHud::m_HelpMessageDisplayForever
static uint16_t *hud_help_message = NULL;   // CHud::m_HelpMessage (UTF-16)

typedef float (*find_ground_z_fn)(float x, float y);
static find_ground_z_fn find_ground_z = NULL;
typedef void *(*find_player_ped_fn)(void);
static find_player_ped_fn find_player_ped = NULL;

// CPed::Teleport takes its CVector BY POINTER, not in s0/s1/s2.
//
// The mangled name says CVector by value, and a three-float aggregate would
// normally travel in the float registers under AAPCS64 -- but the body is
//
//   ldr d0, [x1]  /  ldr s2, [x1, #8]  /  str d0, [x20, #64]
//
// so the argument arrives as an address in x1. Passing three floats instead left
// the callee dereferencing whatever happened to be sitting in x1, which is why
// teleporting never worked no matter which blip we picked. The blip was never
// the bug.
typedef void (*ped_teleport_fn)(void *ped, const float *pos);
static ped_teleport_fn ped_teleport = NULL;

// That same store says where a ped keeps its position: +64 for x and y, +72 for
// z -- the translation row of the matrix that begins at +16.
#define PED_POS 64

// ---- the game's own text ----
//
// Zone names are GXT keys, so rather than shipping a guessed English list the
// menu asks the game for them and falls back only where a key is missing. As a
// side effect the names arrive in whatever language the game is set to.
// Reached through CText::msInstance rather than through TheText(). TheText() is
// a weak symbol and a lazy constructor -- if the slot is empty it allocates a
// brand new, empty CText and installs it, which would answer every key with a
// miss and quietly replace the game's real text object. Reading the pointer is
// both safer and the same thing the game's own inlined accessors do:
// TheText()'s adrp/ldr lands on a GOT entry relocated to CText::msInstance.
// CText::Get is the call the game itself makes -- CCurrentVehicle::Display and
// CZone::GetTranslatedName both use it. CText::Exists, which this used to gate
// on, answered false for every key including CHEAT1, one the game demonstrably
// resolves; whatever it is checking, it is not what Get needs.
typedef uint16_t *(*text_get_fn)(void *self, const char *key);
static void **ctext_instance = NULL;
static text_get_fn text_get = NULL;

// The game's strings are UTF-16. Nothing the menu shows needs more than ASCII,
// and the help box is fed a wide buffer anyway, so anything outside it becomes
// '?' rather than dropping the whole name.
static int wide_to_ascii(const uint16_t *w, char *out, int len) {
  if (!w || !w[0])
    return 0;
  int n = 0;
  for (; w[n] && n < len - 1; n++)
    out[n] = (w[n] < 0x80) ? (char)w[n] : '?';
  out[n] = 0;
  return n > 0;
}

// Fills `out` from the GXT key, or leaves it untouched and returns 0.
static int gxt_lookup(const char *key, char *out, int len) {
  if (!key || !key[0] || !ctext_instance || !text_get)
    return 0;
  void *t = *ctext_instance;
  if (!t)
    return 0;
  return wide_to_ascii(text_get(t, key), out, len);
}

// ---- cheats ----
// Called straight through by symbol. Cheats whose flags nothing in this build
// reads (WallClimbingCheat just eors a byte) are left out rather than listed as
// something that looks like it works.
typedef struct {
  const char *name;
  const char *sym;
  void (*fn)(void);
  int arg;    // 1 = call as fn(1); only HealthCheat takes one, see below
  int cat;    // which list it appears under
} menu_cheat;

// Categories exist because 28 cheats in one column is a scrolling contest. The
// split is by what the cheat acts on, which is the only grouping the names
// themselves suggest.
enum {
  CHEAT_CAT_PLAYER = 0,
  CHEAT_CAT_WANTED,
  CHEAT_CAT_WEATHER,
  CHEAT_CAT_VEHICLE,
  CHEAT_CAT_PEDS,
  CHEAT_NUM_CATS
};

static const char *const cheat_cat_name[CHEAT_NUM_CATS] = {
  "Player", "Wanted level", "Weather & time", "Vehicles", "Peds",
};

static menu_cheat menu_cheats[] = {
  { "Weapons 1",        "_Z12WeaponCheat1v",            NULL, 0, CHEAT_CAT_PLAYER },
  { "Weapons 2",        "_Z12WeaponCheat2v",            NULL, 0, CHEAT_CAT_PLAYER },
  { "Weapons 3",        "_Z12WeaponCheat3v",            NULL, 0, CHEAT_CAT_PLAYER },
  // HealthCheat is the one cheat that takes a parameter, and it gates the game's
  // own on-screen confirmation (tst w0,#0xff / b.eq past the CText work), so
  // pass 1 to get the native message rather than a silent top-up.
  { "Health",           "_Z11HealthCheath",             NULL, 1, CHEAT_CAT_PLAYER },
  { "Armour",           "_Z11ArmourCheatv",             NULL, 0, CHEAT_CAT_PLAYER },
  { "Money",            "_Z10MoneyCheatv",              NULL, 0, CHEAT_CAT_PLAYER },
  { "Wanted up",        "_Z18WantedLevelUpCheatv",      NULL, 0, CHEAT_CAT_WANTED },
  { "Wanted down",      "_Z20WantedLevelDownCheatv",    NULL, 0, CHEAT_CAT_WANTED },
  { "Sunny",            "_Z17SunnyWeatherCheatv",       NULL, 0, CHEAT_CAT_WEATHER },
  { "Extra sunny",      "_Z22ExtraSunnyWeatherCheatv",  NULL, 0, CHEAT_CAT_WEATHER },
  { "Cloudy",           "_Z18CloudyWeatherCheatv",      NULL, 0, CHEAT_CAT_WEATHER },
  { "Rainy",            "_Z17RainyWeatherCheatv",       NULL, 0, CHEAT_CAT_WEATHER },
  { "Foggy",            "_Z17FoggyWeatherCheatv",       NULL, 0, CHEAT_CAT_WEATHER },
  { "Faster time",      "_Z13FastTimeCheatv",           NULL, 0, CHEAT_CAT_WEATHER },
  { "Slower time",      "_Z13SlowTimeCheatv",           NULL, 0, CHEAT_CAT_WEATHER },
  { "Faster weather",   "_Z16FastWeatherCheatv",        NULL, 0, CHEAT_CAT_WEATHER },
  // TankCheat is not "spawn a tank". It walks a counter over the whole vehicle
  // model range (130..216) and hands whatever it lands on to VehicleCheat, so it
  // spawns a different vehicle every time -- which is exactly the random cars
  // that showed up. Spawn vehicle below is the same VehicleCheat call with the
  // model chosen deliberately; this entry stays, honestly named, because the
  // cycling is the game's own cheat.
  { "Random vehicle",   "_Z9TankCheatv",                NULL, 0, CHEAT_CAT_VEHICLE },
  { "Trashmaster",      "_Z16TrashmasterCheatv",        NULL, 0, CHEAT_CAT_VEHICLE },
  { "Chromed cars",     "_Z14GlassCarsCheatv",          NULL, 0, CHEAT_CAT_VEHICLE },
  { "Black cars",       "_Z14BlackCarsCheatv",          NULL, 0, CHEAT_CAT_VEHICLE },
  { "Pink cars",        "_Z13PinkCarsCheatv",           NULL, 0, CHEAT_CAT_VEHICLE },
  { "Tiny bike wheels",      "_Z15BikeWheelsCheatv",         NULL, 0, CHEAT_CAT_VEHICLE },
  { "Big heads",        "_Z13BigHeadsCheatv",           NULL, 0, CHEAT_CAT_PEDS },
  { "Blow up cars",     "_Z15BlowUpCarsCheatv",         NULL, 0, CHEAT_CAT_VEHICLE },
  { "Mad drivers",      "_Z12MadCarsCheatv",            NULL, 0, CHEAT_CAT_VEHICLE },
  { "Peds riot",        "_Z11MayhemCheatv",             NULL, 0, CHEAT_CAT_PEDS },
  { "Peds attack you",  "_Z27EverybodyAttacksPlayerCheatv",  NULL, 0, CHEAT_CAT_PEDS },
  { "Peds have weapons","_Z21DoChicksWithGunsCheatv",   NULL, 0, CHEAT_CAT_PEDS },
};
#define MENU_NUM_CHEATS ((int)(sizeof(menu_cheats) / sizeof(menu_cheats[0])))

static int cheats_ready = 0;

// ---- the flying ceiling, second attempt ----
//
// The user's read of this was right and mine was wrong: it is not a property of
// flying, it is a height limit the game keeps in a variable. The CLEO
// "Unlimited Flying" script is eighteen bytes and all it does is write the float
// 8000.0 to one address and end.
//
// That address in this build is CVehicle::rcHeliHeightLimit -- a named, exported
// four-byte float, which FlyingControl loads and compares the vehicle's z
// against before any of the hardcoded 80.0 business:
//
//   ldr  x9, [x9, #1200]      ; -> CVehicle::rcHeliHeightLimit
//   ldr  s2, [x9]
//   ldr  s1, [x19, #72]       ; z
//   fcmp s1, s2
//
// So the toggle is now a four-byte write to a variable that exists for exactly
// this purpose. No code patching, and nothing borrowed from a neighbour: the
// last attempt parked two floats in VehicleNames and took every vehicle's
// collision with it.
// Writing 8000.0 there on its own changed nothing, and the log shows why: the
// stock limit is 60.0, and the two branches of FlyingControl work out to the
// same formula.
//
//   if (z > rcHeliHeightLimit)  lift *= 10 / ((z - limit) - 10)
//   else if (z > 80.0)          lift *= 10 / (z - 70)
//
// With limit 60 the first branch gives 10/(z-70). With limit 8000 the first
// branch never fires and the second gives 10/(z-70). Identical. The 80/-70 pair
// is the real ceiling, and it is two MOVZ immediates in the code.
//
// So both halves are needed: patch those immediates once at init to 300/-290 --
// plain instruction rewriting, no borrowed storage this time -- and have the
// toggle push rcHeliHeightLimit out of the way so the patched branch is the one
// that runs. Toggled off, the limit goes back to 60, the first branch governs
// again, and the patched constants are unreachable, so stock behaviour is exact.
#define FLY_OFF_CAP   0x69c
#define FLY_OFF_FLOOR 0x6ac

#define MOVZ_W8_HI(imm16) (0x52a00000u | ((uint32_t)(imm16) << 5) | 8u)
#define FLY_CAP_STOCK   MOVZ_W8_HI(0x42a0)   //   80.0
#define FLY_FLOOR_STOCK MOVZ_W8_HI(0xc28c)   //  -70.0
#define FLY_CAP_HIGH    MOVZ_W8_HI(0x4396)   //  300.0
#define FLY_FLOOR_HIGH  MOVZ_W8_HI(0xc391)   // -290.0

// Runs from menu_init, while the image is still only mapped at load_base.
static void fly_raise_hard_cap(void) {
  const uintptr_t fc =
      so_try_find_addr_rx(&game_mod, "_ZN8CVehicle13FlyingControlE12eFlightModel");
  if (!fc || !game_mod.load_base || !game_mod.load_virtbase)
    return;

  const uintptr_t virt = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t base = (uintptr_t)game_mod.load_base;
  uint32_t *const cap = (uint32_t *)(base + (fc + FLY_OFF_CAP - virt));
  uint32_t *const floor_ = (uint32_t *)(base + (fc + FLY_OFF_FLOOR - virt));

  if (*cap != FLY_CAP_STOCK || *floor_ != FLY_FLOOR_STOCK) {
    debugPrintf("MENU: flying cap NOT patched, found %08x %08x\n", *cap, *floor_);
    return;
  }
  *cap = FLY_CAP_HIGH;
  *floor_ = FLY_FLOOR_HIGH;
  debugPrintf("MENU: flying cap 80 -> 300 (only reached while Fly higher is on)\n");
}

#define FLY_LIMIT_HIGH 8000.0f

static float *fly_height_limit = NULL;
static float fly_height_stock = 0.0f;
static int fly_height_saved = 0;
static int fly_height_applied = 0;

static void fly_limit_set(int high) {
  if (!fly_height_limit)
    return;

  if (!fly_height_saved) {
    fly_height_stock = *fly_height_limit;
    fly_height_saved = 1;
    debugPrintf("MENU: rcHeliHeightLimit was %.1f\n", (double)fly_height_stock);
  }

  *fly_height_limit = high ? FLY_LIMIT_HIGH : fly_height_stock;
  fly_height_applied = high;
  debugPrintf("MENU: rcHeliHeightLimit -> %.1f\n", (double)*fly_height_limit);
}

// ---- the first attempt at the flying ceiling, and why it is gone ----
//
// "Fly higher" patched CVehicle::FlyingControl to read its altitude cap from two
// floats instead of holding them as MOVZ immediates, and parked those floats in
// VehicleNames -- 0x46e bytes of .data I had convinced myself nothing used.
//
// Nothing used it *in the same translation unit*. That is all I actually
// checked: a grep for `adrp x8, 81a000` paired with `add #0x1c8`. Cross-module
// access goes through the GOT, and `objdump -R` lists a GLOB_DAT relocation for
// VehicleNames with five readers (0x343b6c, 0x3a07e8, 0x474268, 0x4ba4d4,
// 0x4ba604). It sits between HandlingFilename and BOAT_BUOYANCY_DAMPING, in the
// middle of the vehicle handling data.
//
// So the menu wrote floats over live vehicle data at init and then every frame,
// and every vehicle in the world -- ours and the game's own traffic -- lost its
// collision and fell through the map. Spawned cars falling was that, and so was
// the empty street. The user spotted the correlation ("this only started when
// you added the fly higher limit") long before I did; I was busy proving the
// spawn code innocent, which it was.
//
// The 80.0 cap is real and at FlyingControl+0x69c if this is ever revisited.
// What it needs is somewhere safe to keep two floats, and **"I grepped and found
// no readers" is not a proof of that** -- check `objdump -R` for a GOT entry
// before believing any data symbol is dead.

// ---- always-on toggles ----
//
// CPlayerPed::RestoreSprintEnergy (0x45cc28) reads the current sprint energy
// from +3116 and its maximum from +3120, topping the first up towards the
// second. Rather than trickle it in, the toggle pins one to the other.
#define PED_SPRINT_ENERGY 3116
#define PED_SPRINT_MAX    3120

// Health and armour are floats on the player ped. HealthCheat stores to +1440
// (str s8, [x0, #1440] at 0x341488) and ArmourCheat to +1444 (0x34233c), both
// on the pointer FindPlayerPed returns -- so they can be read and written
// directly, which is what regen and invincibility need.
#define PED_HEALTH  1440
#define PED_ARMOUR  1444
#define PED_MAX     100.0f

// CPed::SetAmmo(type, count) resolves the weapon slot through
// CWeaponInfo::GetWeaponInfo and returns early when the type has none, so
// refilling by type is safe: slots the player does not own are skipped by the
// game's own check rather than by us poking the ped struct.
#define WEAPON_TYPE_MAX 36
#define AMMO_TOPUP      9999

typedef void (*set_ammo_fn)(void *ped, int weapon_type, unsigned count);
static set_ammo_fn ped_set_ammo = NULL;

// Turning unlimited ammo off used to leave everything at 9999, because nothing
// remembered what you had. CPed::SetAmmo shows where it goes:
//
//   x0 = CWeaponInfo::GetWeaponInfo(type)
//   ldrsw x8, [x0, #96]        ; the ped's weapon slot for that type, -1 if none
//   madd  x10, x8, #28, x19    ; ped + slot * 28
//   str   w20, [x10, #1716]    ; ammo
//
// so the counts can be read back the same way and put back on the way out.
// Snapshotting per slot rather than per type matters: several types share a
// slot, and saving per type would restore whichever one happened to be last.
typedef void *(*get_weapon_info_fn)(int weapon_type);
static get_weapon_info_fn get_weapon_info = NULL;

#define WEAPONINFO_SLOT   96
#define PED_WEAPON_BASE   1716
#define PED_WEAPON_STRIDE 28
#define MAX_WEAPON_SLOTS  32

static int ammo_backup[MAX_WEAPON_SLOTS];
static uint8_t ammo_backup_valid[MAX_WEAPON_SLOTS];
static int ammo_was_on = 0;

static int weapon_slot_of(int type) {
  if (!get_weapon_info)
    return -1;
  const void *info = get_weapon_info(type);
  if (!info)
    return -1;
  const int slot = *(const int32_t *)((uintptr_t)info + WEAPONINFO_SLOT);
  return (slot >= 0 && slot < MAX_WEAPON_SLOTS) ? slot : -1;
}

static int *ammo_cell(void *ped, int slot) {
  return (int *)((uintptr_t)ped + PED_WEAPON_BASE +
                 (size_t)slot * PED_WEAPON_STRIDE);
}

static void ammo_snapshot(void *ped) {
  memset(ammo_backup_valid, 0, sizeof(ammo_backup_valid));
  int n = 0;
  for (int t = 1; t <= WEAPON_TYPE_MAX; t++) {
    const int slot = weapon_slot_of(t);
    if (slot < 0 || ammo_backup_valid[slot])
      continue;
    ammo_backup[slot] = *ammo_cell(ped, slot);
    ammo_backup_valid[slot] = 1;
    n++;
  }
  debugPrintf("MENU: saved ammo for %d weapon slots\n", n);
}

static void ammo_restore(void *ped) {
  int n = 0;
  for (int slot = 0; slot < MAX_WEAPON_SLOTS; slot++) {
    if (!ammo_backup_valid[slot])
      continue;
    *ammo_cell(ped, slot) = ammo_backup[slot];
    n++;
  }
  debugPrintf("MENU: restored ammo for %d weapon slots\n", n);
}

// CWanted lives at ped+0x928: WantedLevelDownCheat does FindPlayerPed(),
// add x0,#0x928, then CheatWantedLevel(0) -- which is exactly "clear it".
#define PED_WANTED_OFFSET 0x928
typedef void (*cheat_wanted_fn)(void *wanted, int level);
static cheat_wanted_fn cheat_wanted_level = NULL;

typedef enum {
  MENU_TOG_NEVER_TIRED = 0,
  MENU_TOG_REGEN,
  MENU_TOG_INVINCIBLE,
  MENU_TOG_AMMO,
  MENU_TOG_NEVER_WANTED,
  MENU_TOG_FLY_HIGHER,
  MENU_NUM_TOGGLES
} menu_toggle;

static const char *const menu_toggle_name[MENU_NUM_TOGGLES] = {
  "Never tired",
  "Regen health & armour",
  "Invincible",
  "Unlimited ammo",
  "Never wanted",
  "Fly higher",
};

static int menu_toggle_on[MENU_NUM_TOGGLES];

// Save on the way in, put back on the way out. Kept out of the slow tick above
// so the snapshot happens the instant the toggle flips, before anything has
// been overwritten with 9999.
static void menu_apply_ammo_edges(void *ped) {
  const int on = menu_toggle_on[MENU_TOG_AMMO];
  if (on == ammo_was_on)
    return;
  ammo_was_on = on;

  if (on)
    ammo_snapshot(ped);
  else
    ammo_restore(ped);
}

// Runs every frame while the game is live, menu open or not.
static void menu_apply_toggles(void) {
  const int want_high = menu_toggle_on[MENU_TOG_FLY_HIGHER];
  if (want_high != fly_height_applied)
    fly_limit_set(want_high);

  if (!find_player_ped)
    return;

  void *ped = find_player_ped();
  if (!ped)
    return;

  if (menu_toggle_on[MENU_TOG_NEVER_TIRED]) {
    float *energy = (float *)((uintptr_t)ped + PED_SPRINT_ENERGY);
    const float *max = (const float *)((uintptr_t)ped + PED_SPRINT_MAX);
    if (*energy < *max)
      *energy = *max;
  }

  menu_apply_ammo_edges(ped);

  float *health = (float *)((uintptr_t)ped + PED_HEALTH);
  float *armour = (float *)((uintptr_t)ped + PED_ARMOUR);

  // Invincible pins both every frame; regen tops them up once a second, so you
  // still take damage and recover rather than never flinching.
  if (menu_toggle_on[MENU_TOG_INVINCIBLE]) {
    *health = PED_MAX;
    *armour = PED_MAX;
  }

  // Every frame, so stars never get a chance to appear rather than flickering
  // on and being cleared a second later.
  if (menu_toggle_on[MENU_TOG_NEVER_WANTED] && cheat_wanted_level)
    cheat_wanted_level((void *)((uintptr_t)ped + PED_WANTED_OFFSET), 0);

  static int slow_tick = 0;
  if (++slow_tick < 60)
    return;
  slow_tick = 0;

  if (menu_toggle_on[MENU_TOG_REGEN]) {
    if (*health < PED_MAX)
      *health = PED_MAX;
    if (*armour < PED_MAX)
      *armour = PED_MAX;
  }

  if (menu_toggle_on[MENU_TOG_AMMO] && ped_set_ammo) {
    for (int t = 1; t <= WEAPON_TYPE_MAX; t++)
      ped_set_ammo(ped, t, AMMO_TOPUP);
  }
}


// ---- teleport ----
//
// Fixed destinations rather than the map marker. Hunting the marker down in
// ms_RadarTrace was never the problem -- CPed::Teleport's calling convention
// was -- but a named list is more useful than a marker anyway: it works with the
// map closed and needs no setup.
//
// The destinations are the game's own map zones. gtalcs.teleport.csi carried
// each zone as a pair of opposing corners and teleported to the centre of the
// box, snapped to the ground; these are those boxes reduced to their centres.
// Names are GXT keys wherever the zone has one, so they come out of the game
// already localised, and `fallback` only shows if a key is missing.
typedef struct {
  const char *key;        // GXT key, or NULL for places the game does not name
  const char *fallback;
  float x, y;
} menu_place;

// Roughly south to north, so walking the list walks the city.
static const menu_place menu_places[] = {
  { "PORT_W",   "Portland Harbor",         908.68f, -1068.47f },
  { "PORT_S",   "Callahan Point",         1283.88f, -1160.74f },
  { "PORT_E",   "Atlantic Quays",         1589.68f,  -841.65f },
  { "PORT_I",   "Trenton",                1214.63f,  -905.95f },
  { "S_VIEW",   "Portland View",          1214.85f,  -627.31f },
  { "CHINA",    "Chinatown",               905.42f,  -685.99f },
  { "REDLIGH",  "Red Light District",      905.38f,  -373.12f },
  { "TOWERS",   "Hepburn Heights",         905.42f,  -180.58f },
  { "LITTLEI",  "Saint Marks",            1227.40f,  -295.32f },
  { "HARWOOD",  "Harwood",                1067.48f,   122.25f },
  { "EASTBAY",  "Portland Beach",         1593.48f,  -206.92f },
  { "IND_ZON",  "Portland",               1259.91f,  -447.80f },
  { NULL,       "Portland police",        1159.08f,  -663.02f },
  { NULL,       "Portland hospital",      1159.09f,  -565.57f },
  { NULL,       "Markos bistro",          1376.47f,  -562.94f },
  { "ROADBR2",  "Callahan Bridge",         529.82f,  -933.30f },
  { "CONSTRU",  "Fort Staunton",           427.10f,  -236.62f },
  { "STADIUM",  "Aspatria",                -54.76f,  -126.05f },
  { "YAKUSA",   "Torrington",              388.77f, -1366.18f },
  { "SHOPING",  "Bedford Point",           -12.44f, -1338.25f },
  { "COM_EAS",  "Newport",                 407.61f,  -735.69f },
  { "PARK",     "Belleville Park",          38.85f,  -708.07f },
  { "UNIVERS",  "Liberty Campus",          178.27f,  -236.62f },
  { "HOSPI_2",  "Rockford",                366.24f,   103.89f },
  { "BIG_DAM",  "Cochrane Dam",          -1131.01f,   398.99f },
  { "AIRPORT",  "Francis Intl Airport",  -1050.80f,  -806.58f },
  { "PROJECT",  "Wichita Gardens",        -591.44f,   -87.67f },
  { "SWANKS",   "Cedar Grove",            -567.07f,   371.72f },
  { "SUB_IND",  "Pike Creek",            -1109.94f,   -87.61f },
};
#define MENU_NUM_PLACES ((int)(sizeof(menu_places) / sizeof(menu_places[0])))

// The game's own zone table, which is the authority on both where a zone is and
// what it is called. The coordinates above came from reading a CLEO script's
// data, and teleporting to "Atlantic Quays" put the player in Portland Harbor,
// so that reading was wrong somewhere -- most likely the mapping from menu entry
// to coordinate block. Rather than re-guess it, ask the game.
//
// gpTheZones is a CTheZones*. CZone is 72 bytes: an 8-byte label at +0, the box
// minimum at +8/+12/+16 and its maximum at +20/+24/+28, per
// CTheZones::PointLiesWithinZone.
#define ZONE_STRIDE 72
#define ZONE_MIN     8
#define ZONE_MAX    20

// Landing on the road rather than on the geometry directly under the zone's
// centre. FindGroundZForCoord returns whatever surface is highest at that x/y,
// so a zone whose middle happens to be a building -- Saint Mark's, for one --
// put the player on its roof.
//
// The path network has no such problem: every node is a road. VehicleCheat uses
// the same call to place a car, and reads the node back the same way -- node
// records are 20 bytes with x, y and z as int16 at +4, +6 and +8, each an eighth
// of a world unit (its scvtf is followed by fmul by 0.125).
#define PATHNODE_STRIDE 20
#define PATHNODE_X       4
#define PATHNODE_SCALE   0.125f
#define PATHNODE_SEARCH  200.0f

typedef int (*find_node_fn)(void *paths, const float *coors, unsigned char type,
                            float dist, int a, int b, int c, int d);
static void **gp_the_paths = NULL;
static find_node_fn find_node_closest = NULL;

// Puts the nearest road position to x/y in `out`, or returns 0.
static int road_near(float x, float y, float z, float *out) {
  if (!gp_the_paths || !find_node_closest)
    return 0;
  void *paths = *gp_the_paths;
  if (!paths)
    return 0;

  const float coors[3] = { x, y, z };
  const int idx = find_node_closest(paths, coors, 0, PATHNODE_SEARCH, 0, 0, 0, 0);
  if (idx < 0)
    return 0;

  const uint8_t *nodes = *(const uint8_t **)paths;
  if (!nodes)
    return 0;

  const int16_t *p =
      (const int16_t *)(nodes + (size_t)idx * PATHNODE_STRIDE + PATHNODE_X);
  out[0] = p[0] * PATHNODE_SCALE;
  out[1] = p[1] * PATHNODE_SCALE;
  out[2] = p[2] * PATHNODE_SCALE;
  return 1;
}

typedef short (*find_zone_label_fn)(void *zones, char *label, int type);
typedef void *(*get_zone_fn)(void *zones, unsigned short index);
typedef uint16_t *(*zone_name_fn)(void *zone);

static void **gp_the_zones = NULL;
static find_zone_label_fn find_zone_by_label = NULL;
static get_zone_fn get_nav_zone = NULL;
static get_zone_fn get_info_zone = NULL;
static get_zone_fn get_map_zone = NULL;
static zone_name_fn zone_translated_name = NULL;

// Which of the four zone types holds the named zones is not obvious from the
// disassembly -- the label search branches on type and each type has its own
// array and accessor -- so all three are tried and the one that answers is
// logged.
static const struct { int type; const char *what; } zone_types[] = {
  { 0, "navigation" }, { 3, "map" }, { 2, "info" },
};
#define NUM_ZONE_TYPES ((int)(sizeof(zone_types) / sizeof(zone_types[0])))

static get_zone_fn zone_getter(int type) {
  switch (type) {
    case 3:  return get_map_zone;
    case 2:  return get_info_zone;
    default: return get_nav_zone;
  }
}

// Fills in the centre of the named zone and, where the game has one, its
// display name. Returns 0 if the game does not know the label.
static int zone_lookup(const char *key, float *cx, float *cy,
                       char *name, int name_len) {
  if (!key || !gp_the_zones || !find_zone_by_label)
    return 0;
  void *zones = *gp_the_zones;
  if (!zones)
    return 0;

  // The label is copied because the search writes through its argument.
  char label[16];
  snprintf(label, sizeof(label), "%s", key);

  for (int t = 0; t < NUM_ZONE_TYPES; t++) {
    const get_zone_fn get = zone_getter(zone_types[t].type);
    if (!get)
      continue;

    const short idx = find_zone_by_label(zones, label, zone_types[t].type);
    if (idx < 0)
      continue;

    const uint8_t *z = (const uint8_t *)get(zones, (unsigned short)idx);
    if (!z)
      continue;

    const float *mn = (const float *)(z + ZONE_MIN);
    const float *mx = (const float *)(z + ZONE_MAX);
    *cx = (mn[0] + mx[0]) * 0.5f;
    *cy = (mn[1] + mx[1]) * 0.5f;

    // CZone::GetTranslatedName goes through the same CText the HUD uses to
    // print the zone you are standing in, so it sidesteps our own GXT lookup.
    if (name && zone_translated_name) {
      const uint16_t *w = zone_translated_name((void *)z);
      if (w && w[0]) {
        int n = 0;
        for (; w[n] && n < name_len - 1; n++)
          name[n] = (w[n] < 0x80) ? (char)w[n] : '?';
        name[n] = 0;
      }
    }

    debugPrintf("MENU: zone \"%s\" found as %s zone %d, centre %.1f, %.1f\n",
                key, zone_types[t].what, (int)idx, *cx, *cy);
    return 1;
  }

  debugPrintf("MENU: zone \"%s\" not found in any zone type\n", key);
  return 0;
}

// Resolved the first time the list is opened rather than in menu_init: neither
// the zones nor CText are up that early.
// Which island a place is on. Asked of the game rather than sorted by hand:
// CTheZones::GetLevelFromPosition returns the eLevelName, which is exactly the
// first/second/third city split.
enum { PLACE_CAT_PORTLAND = 0, PLACE_CAT_STAUNTON, PLACE_CAT_SHORESIDE,
       PLACE_NUM_CATS };

static const char *const place_cat_name[PLACE_NUM_CATS] = {
  "1 - Portland", "2 - Staunton Island", "3 - Shoreside Vale",
};

// Takes gpTheZones as `this`; the vector is the second argument. Passing the
// vector as `this` returned garbage levels and then faulted outright.
typedef int (*level_from_pos_fn)(void *zones, const float *pos);
static level_from_pos_fn level_from_position = NULL;

// ---- the map marker ----
//
// This was dropped once as unfixable. It was not: the marker scan was always
// right, and what broke it was CPed::Teleport's CVector arriving by pointer
// rather than in the float registers. With that understood it is worth having
// back, so the marker sits at the top of the teleport list.
//
// CRadar::ms_RadarTrace is 75 entries of 60 bytes. CRadar::SetTargetBlip walks
// the array testing +43 for a free slot and writes the position at +12; +40 is
// the blip type, where 4 is a coordinate blip -- the kind a marker you place on
// the map is, as opposed to the char blip (2) a mission contact gets.
#define BLIP_STRIDE 60
#define BLIP_COUNT  75
#define BLIP_INUSE  43
#define BLIP_POS    12

// The marker *you* place, as opposed to the ones the game puts on the map for
// home, missions and shops. CRadar::SetTargetBlip stamps sprite 49 at +56:
//
//   mov  w12, #0x31            ; 49
//   strh w12, [x8, #56]
//
// and no other blip gets that sprite. The previous version matched on +40 being
// 4, which is why it happily teleported to the home marker: +40 is not a blip
// type at all, it is a generation counter that SetTargetBlip reads, increments
// and writes back. Small values there meant nothing.
#define BLIP_SPRITE          56
#define BLIP_SPRITE_WAYPOINT 49

static uint8_t *radar_trace = NULL;

// The marker you place is not a radar blip at all.
//
// The blip dump settled that: with a marker on the map, ms_RadarTrace held
// exactly two entries -- sprite 40 next to the player and sprite 19 on the home
// icon, which is the one it kept teleporting to. Nothing for the waypoint.
// CRadar::MapWayPoint is 8 bytes and looks right, but only CRadar::Shutdown and
// LoadAllRadarBlips touch it, so it is not where a live marker lives either.
//
// CMenuManager keeps it: m_TargetIsOn is a byte saying whether one is placed and
// m_fTargetPos is the two floats. Both are logged, so if the coordinates turn
// out to be map-space rather than world-space that will be obvious rather than
// mysterious.
static uint8_t *menu_target_on = NULL;
static float *menu_target_pos = NULL;
static float *radar_map_waypoint = NULL;   // CRadar::MapWayPoint, two floats
static int *menu_target_blip_index = NULL;
// The map target, two floats inside the RadarMap object.
#define RADARMAP_TARGET 128

static void **gp_radar_map = NULL;   // GRadarMap

// Where the marker you place actually lives is still unknown, so this reports
// every candidate at once rather than guessing at a fourth.
//
// Ruled out so far, each by looking rather than reasoning:
//   - ms_RadarTrace holds only two blips with a marker placed, sprite 40 by the
//     player and sprite 19 on the home icon. The array is the right one: it is
//     0x1194 bytes, exactly 75 x 60, and +43 is the in-use byte per
//     CRadar::SetTargetBlip's own free-slot scan.
//   - CMenuManager::m_TargetIsOn stays 0 and m_fTargetPos stays 0,0.
//   - CRadar::MapWayPoint and m_TargetBlipIndex have no writers at all outside
//     save/load -- checked for both GOT access and same-module addressing.
//
// Something draws that pin on the map and on the radar, so the data exists. The
// blip sweep below now reports any slot with a non-zero position regardless of
// its in-use byte, in case the marker is stored with a different convention.
// Liberty City fits comfortably inside +/-3000 on both axes, so anything
// outside that is not a place -- it is a field that happens to be non-zero.
//
// CRadar::MapWayPoint read back as -12921941685960704.0, my "non-zero means
// valid" test accepted it, and the teleport that followed took the game down.
// Any coordinate that comes from guessing at a field has to be checked before it
// reaches CPed::Teleport, and honestly all of them should be.
#define WORLD_LIMIT 3000.0f

static int menu_pos_sane(float x, float y) {
  // NaN fails every comparison, which is what the first test is for.
  if (!(x == x) || !(y == y))
    return 0;
  return x > -WORLD_LIMIT && x < WORLD_LIMIT &&
         y > -WORLD_LIMIT && y < WORLD_LIMIT;
}

static int menu_find_marker(float *out_x, float *out_y) {
  // Printed with %g, not %.1f. The last build showed MapWayPoint as "-0.0" and I
  // read that as zero; it was a tiny non-zero value rounding to -0.0, so the
  // "is it non-zero" test passed, the range check waved it through -- the origin
  // is a perfectly ordinary coordinate -- and the teleport landed at 0,0, which
  // is Aspatria. Rounded output hid the one digit that mattered.
  if (radar_map_waypoint)
    debugPrintf("MENU: CRadar::MapWayPoint = %g, %g\n",
                (double)radar_map_waypoint[0], (double)radar_map_waypoint[1]);
  if (menu_target_blip_index)
    debugPrintf("MENU: m_TargetBlipIndex = %d\n", *menu_target_blip_index);
  if (menu_target_on && menu_target_pos)
    debugPrintf("MENU: m_TargetIsOn=%u m_fTargetPos = %g, %g\n",
                *menu_target_on, (double)menu_target_pos[0],
                (double)menu_target_pos[1]);

  // MapWayPoint is *not* a source. It has no writers anywhere outside Shutdown
  // and LoadAllRadarBlips, and what it holds is whatever was left in that
  // memory -- garbage that has already, once as -1.29e16 and once as a hair
  // either side of zero, been mistaken for a destination.

  // The map screen is a class of its own: RadarMap, reached through the global
  // GRadarMap. That is how the CLEO teleport script found the marker -- the
  // string "GRadarMap" sits at the end of the script, right after "Map Marker",
  // and it resolves the symbol and reads an offset out of the object.
  //
  // RadarMap::Update stores an eight-byte pair at +128 and single floats at
  // +108/+112/+116/+124, which are the centre and zoom in some order. Rather
  // than guess which is the target, dump the object and read the answer off the
  // numbers: with a marker placed, its coordinates are in here somewhere and
  // they will look like the world coordinates they are.
  // Found it. Dumping the object next to the player's position made it obvious:
  // with a marker placed north-west of a player at 1117.7, -1104.6, GRadarMap
  // +128 held 818.8, -492.8. Everything else in there was zero, a denormal, or
  // a zoom factor.
  //
  // It is the target rather than the map centre: RadarMap::MoveMapCenter writes
  // +108, +112, +140 and +144, and never +128. RadarMap::Update writes +128 as a
  // single eight-byte store, which is what a CVector2D looks like.
  if (gp_radar_map && *gp_radar_map) {
    const float *target = (const float *)((uintptr_t)*gp_radar_map + RADARMAP_TARGET);
    debugPrintf("MENU: GRadarMap target = %g, %g\n", (double)target[0],
                (double)target[1]);

    // Clearing the marker leaves +128 holding the old coordinates, so it still
    // teleports. Something else must say whether one is set, and nothing in
    // RadarMap writes a flag near the target -- there are no stores to +132 or
    // +136 anywhere in the class.
    //
    // So dump the object as raw words. Doing this once with a marker and once
    // without makes the difference a diff rather than a guess, which is how the
    // target itself was found.
    const uint32_t *w = (const uint32_t *)*gp_radar_map;
    for (int off = 0; off < 256; off += 32)
      debugPrintf("MENU: GRadarMap+%3d: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                  off, w[off / 4 + 0], w[off / 4 + 1], w[off / 4 + 2],
                  w[off / 4 + 3], w[off / 4 + 4], w[off / 4 + 5],
                  w[off / 4 + 6], w[off / 4 + 7]);
    if (menu_pos_sane(target[0], target[1]) &&
        (target[0] != 0.0f || target[1] != 0.0f)) {
      *out_x = target[0];
      *out_y = target[1];
      return 1;
    }
  }

  if (menu_target_on && menu_target_pos && *menu_target_on &&
      menu_pos_sane(menu_target_pos[0], menu_target_pos[1])) {
    *out_x = menu_target_pos[0];
    *out_y = menu_target_pos[1];
    return 1;
  }

  if (!radar_trace)
    return 0;

  int found = 0;
  for (int i = 0; i < BLIP_COUNT; i++) {
    const uint8_t *b = radar_trace + (size_t)i * BLIP_STRIDE;
    const float x = *(const float *)(b + BLIP_POS);
    const float y = *(const float *)(b + BLIP_POS + 4);
    if (!b[BLIP_INUSE] && x == 0.0f && y == 0.0f)
      continue;

    const uint16_t sprite = *(const uint16_t *)(b + BLIP_SPRITE);
    debugPrintf("MENU: blip %2d use=%u sprite=%u at %g, %g\n", i,
                b[BLIP_INUSE], sprite, (double)x, (double)y);

    if (b[BLIP_INUSE] && sprite == BLIP_SPRITE_WAYPOINT &&
        menu_pos_sane(x, y)) {
      *out_x = x;
      *out_y = y;
      found = 1;
    }
  }
  return found;
}

static char place_label[MENU_NUM_PLACES][40];
static float place_x[MENU_NUM_PLACES];
static float place_y[MENU_NUM_PLACES];
static int place_cat[MENU_NUM_PLACES];
static int places_labelled = 0;

static void menu_label_places(void) {
  if (places_labelled)
    return;
  places_labelled = 1;

  int from_zone = 0, from_gxt = 0;
  for (int i = 0; i < MENU_NUM_PLACES; i++) {
    // Start from the scraped table, then let the game overrule it.
    place_x[i] = menu_places[i].x;
    place_y[i] = menu_places[i].y;
    snprintf(place_label[i], sizeof(place_label[i]), "%s", menu_places[i].fallback);

    char name[40];
    name[0] = 0;
    if (zone_lookup(menu_places[i].key, &place_x[i], &place_y[i],
                    name, sizeof(name))) {
      from_zone++;
      if (name[0])
        snprintf(place_label[i], sizeof(place_label[i]), "%s", name);
    } else if (gxt_lookup(menu_places[i].key, name, sizeof(name))) {
      from_gxt++;
      snprintf(place_label[i], sizeof(place_label[i]), "%s", name);
    }

    // eLevelName is 1-based with 0 meaning "no level", so anything the game
    // will not place falls back to Portland rather than off the end of the
    // category array.
    place_cat[i] = PLACE_CAT_PORTLAND;
    if (level_from_position && gp_the_zones) {
      const float pos[3] = { place_x[i], place_y[i], 0.0f };
      void *zones = gp_the_zones ? *gp_the_zones : NULL;
      const int level = zones ? level_from_position(zones, pos) : 0;
      if (level >= 1 && level <= PLACE_NUM_CATS)
        place_cat[i] = level - 1;
      else
        debugPrintf("MENU: \"%s\" has level %d, filed under Portland\n",
                    place_label[i], level);
    }
  }

  debugPrintf("MENU: %d/%d places located from the game's zones (%d named by GXT)\n",
              from_zone, MENU_NUM_PLACES, from_gxt);
  debugPrintf("MENU: CText::msInstance = %p, gpTheZones = %p\n",
              ctext_instance ? *ctext_instance : NULL,
              gp_the_zones ? *gp_the_zones : NULL);
}

// ---- vehicle spawner ----
//
// VehicleCheat(modelId) looked like the whole spawner, and it is the call
// TankCheat makes -- but it is only safe for two of the four vehicle classes.
// It picks the class by model id alone:
//
//   sub w8, w19, #0xca / cmp w8, #8 / b.hi   ->  CBike for 202..210,
//                                               CAutomobile for everything else
//
// so a boat gets built as a CAutomobile and the game dies the moment anything
// touches it. That is exactly what happened with the Reefer and the Speeder.
// (TankCheat gets away with it because it skips planes and never lands on a
// boat often enough to matter.)
//
// It also places the vehicle on the nearest path node within 100 units of the
// player, which is why cars kept appearing out of view or across the street.
//
// So the spawn is done here instead, following VehicleCheat's own sequence --
// request, load, allocate, construct, write the matrix, CWorld::Add -- with the
// class chosen from CModelInfo rather than from an id range, and the position
// taken from the player rather than from a path node.
#define VEH_MATRIX  16    // three 16-byte rotation rows, +16..+63
#define VEH_POS     64    // x,y at +64/+68 and z at +72
#define VEH_FLAGS   88    // entity flags; the status is five bits at bit 4
#define VEH_STATUS_MASK 0x1f0ULL

// Which island the entity is on. SpawnInModel writes the vehicle's at +390;
// CEntity::SetupBigBuilding writes a building's at +126, which is a different
// field and the wrong one to copy for a vehicle.
#define ENTITY_LEVEL 390

// The player's forward vector is the second matrix row: CPlaceable::SetHeading
// builds row0 = (cos, sin, 0) and row1 = (-sin, cos, 0), and GTA faces +y.
#define PED_FORWARD (VEH_MATRIX + 16)
#define SPAWN_AHEAD 5.0f

// Planes and trains are deliberately absent: LCS has no flyable plane -- nothing
// in this build even calls CPlane's constructor -- and trains need rails.
typedef enum { VEH_AUTO = 0, VEH_BIKE, VEH_BOAT, VEH_HELI } veh_kind;

// SpawnInModel(int model, CVector &pos) is the game's own vehicle spawner, and
// hand-rolling one instead was the whole problem. Ours built the vehicle and
// added it to the world, and it fell straight through the map -- x and y frozen,
// z going 11.7, 10.5, 4.6, -6.4, -22.5, -43.5 until CWorld::RemoveFallenCars
// swept it up below -100. Boats did not even manage that; their z came out NaN.
//
// The game's version does a dozen things ours did not, and rather than guess
// which one grants collision, this now calls it. Among the differences:
//
//   RequestModel(id, 4)          not 1
//   constructors take createdBy 2, not 1
//   CBike / CHeli / CBoat / CAutomobile chosen by the Is*Model predicates,
//     with CHeli::ActivateHeli(false) and a bike-specific flag at +1540
//   z = FindGroundZForCoord + CEntity::GetDistanceFromCentreOfMassToBaseOfModel
//     -- the model's own height, rather than our flat +1.0
//   CCarCtrl::JoinCarWithRoadSystem
//   several flag writes at +717, and the level byte at +390, not +126
//
// The CVector is by reference and is written back with the settled position, so
// the log shows where the vehicle actually ended up.
typedef void (*spawn_in_model_fn)(int model, float *pos);
static spawn_in_model_fn spawn_in_model = NULL;

// The one that works. TankCheat and TrashmasterCheat both go through it.
typedef void (*vehicle_cheat_fn)(int model);
static vehicle_cheat_fn vehicle_cheat = NULL;

static const char *const veh_kind_name[] = { "Car", "Bike", "Boat", "Helicopter" };

// Streaming and classification are still ours -- the list is built from the
// model table and peds are loaded the same way -- but nothing here constructs a
// vehicle any more. SpawnInModel does that, and does it correctly.
typedef void *(*get_model_info_fn)(const char *name, int *id_out);
typedef void (*request_model_fn)(int model, int flags);
typedef void (*load_all_models_fn)(int prio);
typedef char (*is_model_fn)(int model);

static get_model_info_fn get_model_info = NULL;
static request_model_fn request_model = NULL;
static load_all_models_fn load_all_models = NULL;
static is_model_fn is_car_model = NULL;
static is_model_fn is_bike_model = NULL;
static is_model_fn is_boat_model = NULL;
static is_model_fn is_heli_model = NULL;
static is_model_fn is_in_cd_image = NULL;

// How many model slots exist, so the table can be walked end to end.
static const int *num_model_infos = NULL;

// Classifies a model id, or returns -1 for anything this menu cannot construct
// (planes, trains, and every non-vehicle model).
static int veh_classify(int id) {
  if (is_bike_model && is_bike_model(id)) return VEH_BIKE;
  if (is_boat_model && is_boat_model(id)) return VEH_BOAT;
  if (is_heli_model && is_heli_model(id)) return VEH_HELI;
  if (is_car_model && is_car_model(id))   return VEH_AUTO;
  return -1;
}

// Model ids are per-build, so the menu resolves them by name:
// CModelInfo::GetModelInfo hashes the name and searches, writing the id out.
// LCS keeps only those hashes -- there is no name table to enumerate, which is
// why this list is the one place a name has to be spelled out. Anything that
// does not resolve is dropped and logged rather than left in the menu doing
// nothing when picked.
// The game names its own vehicles. CCurrentVehicle::Display -- the code behind
// the vehicle name that flashes bottom-right when you get in -- does:
//
//   w8  = vehicle->[124]                 (model index)
//   x20 = CModelInfo::ms_modelInfoPtrs[w8]
//   x1  = x20 + 0x52                     (the GXT key, inline in the model info)
//   CHud::SetVehicleName(CText::Get(x1))
//
// So every vehicle carries its own key at model info +0x52, and CText turns it
// into the real name -- localised, and correct by construction. That replaces
// the hand-written name list this used to carry entirely: no spellings to guess,
// nothing to get wrong, and the twenty numbered entries get proper names.
#define MODELINFO_GXT_KEY 0x52

static void ***ms_model_info_ptrs = NULL;

// The six the game has no name for, identified from screenshots of each one
// spawned. All are helicopters, and all of them fly -- even though their model
// info types them as cars, so the menu builds them as CAutomobile. Flight in
// this engine evidently comes from the handling data, not the model type.
//
// (Models 198 and 199, the two the game does type as helicopters, are the ones
// you cannot fly: they are scripted traffic and take off by themselves.)
//
// Three have no textures in this build and look unfinished, which the labels
// say so the entry is not a wasted trip. Labelling any of them by model number
// was worse than useless: it said car, and what appeared was a Hunter.
// Model 198 crashes the game on spawn. Both it and 199 are typed as
// helicopters and both go through the identical CHeli path, so the class is not
// the problem -- 198 itself is incomplete, like the untextured models nearby.
// The likely mechanism is in the constructor:
//
//   ldr x0, [x19, #112]                     (the clump SetModelIndex installed)
//   bl  CElementGroupModelInfo::FillNodeArray
//
// FillNodeArray walks the model's node hierarchy for the rotor bones, so a model
// with no geometry hands it a null and it dies there. That is a theory, not a
// finding -- what is certain is that this one entry crashes, so it is not
// offered. The collision-model check in menu_spawn_vehicle is the general
// version of the same guard, and the log will say if it ever catches anything.
// Models kept out of the list, and why. All three are things you would pick once
// and regret: a crash, a duplicate, and a helicopter that leaves without you.
static const struct { int id; const char *why; } veh_excluded[] = {
  { 198, "crashes on spawn" },
  { 199, "takes off by itself, cannot be flown" },
};
#define NUM_VEH_EXCLUDED ((int)(sizeof(veh_excluded) / sizeof(veh_excluded[0])))

static int veh_model_excluded(int id) {
  for (int i = 0; i < NUM_VEH_EXCLUDED; i++)
    if (veh_excluded[i].id == id)
      return 1;
  return 0;
}

static const char *veh_exclude_reason(int id) {
  for (int i = 0; i < NUM_VEH_EXCLUDED; i++)
    if (veh_excluded[i].id == id)
      return veh_excluded[i].why;
  return "";
}

// Helicopters the model table calls cars. They fly, so they belong under Air
// even though they are built as CAutomobile.
static int veh_model_is_air(int id) {
  if (id == 164)          // the Dodo: a plane the model table calls a car
    return 1;
  return id >= 211 && id <= 216;
}

// Collision model pointer, from CBaseModelInfo::GetColModelPtr: ldr x0,[x0,#48].
#define MODELINFO_COL_MODEL 48

static const struct { int id; const char *name; } veh_extra_names[] = {
  // 211 and 212 are not the same model twice: DEFAULT.IDE calls them rcgoblin
  // and rcraider. Both ship without textures in this build.
  { 164, "Dodo" },                  // a plane, filed under Air; it cannot fly yet
  { 211, "RC Goblin (no tex)" },
  { 212, "RC Raider (no tex)" },
  { 213, "Hunter" },             // olive attack helicopter, rockets and minigun
  { 214, "Maverick" },           // civilian, cream with a blue stripe
  { 215, "Police Maverick" },    // LCPD markings, tail number P619PD
  { 216, "News Maverick (no tex)" }, // its vcnmav textures are missing
};
#define NUM_VEH_EXTRA_NAMES \
  ((int)(sizeof(veh_extra_names) / sizeof(veh_extra_names[0])))

static const char *veh_extra_name(int id) {
  for (int i = 0; i < NUM_VEH_EXTRA_NAMES; i++)
    if (veh_extra_names[i].id == id)
      return veh_extra_names[i].name;
  return NULL;
}

// The model info record for a model id, or NULL.
static const uint8_t *model_info_for(int id) {
  if (!ms_model_info_ptrs || !num_model_infos)
    return NULL;
  if (id < 0 || id >= *num_model_infos)
    return NULL;
  void **table = *ms_model_info_ptrs;
  return table ? (const uint8_t *)table[id] : NULL;
}

// Whether a model's geometry is actually resident, asked the way the game asks
// it: the seventh entry of CBaseModelInfo's vtable (ldr x8,[x0]; ldr x8,[x8,#48])
// returns the clump, and a null one means the model is not there. Constructing
// a ped or a vehicle against that is a null dereference inside the constructor,
// which is exactly what a crash with no log line before it looks like.
#define MODELINFO_VT_GET_CLUMP 6

static int model_has_clump(int id) {
  const uint8_t *info = model_info_for(id);
  if (!info)
    return 0;
  void *const *vtable = *(void *const *const *)info;
  if (!vtable)
    return 0;
  typedef void *(*get_clump_fn)(const void *self);
  const get_clump_fn get_clump = (get_clump_fn)vtable[MODELINFO_VT_GET_CLUMP];
  return get_clump && get_clump(info) != NULL;
}

// Writes the game's display name for a model, or returns 0.
static int vehicle_game_name(int id, char *out, int len) {
  if (!ms_model_info_ptrs || !num_model_infos)
    return 0;
  if (id < 0 || id >= *num_model_infos)
    return 0;

  void **table = *ms_model_info_ptrs;
  if (!table)
    return 0;
  const uint8_t *info = (const uint8_t *)table[id];
  if (!info)
    return 0;

  const char *key = (const char *)(info + MODELINFO_GXT_KEY);
  return gxt_lookup(key, out, len);
}

// The list actually shown. It is built from the game's model table rather than
// from the names above, so every vehicle in the build is spawnable whether or
// not we know what it is called: the names only decide how an entry is
// labelled. Anything unnamed still appears, as "Car 173" or "Boat 191", and is
// spawned exactly the same way.
// `cat` is what the list groups by and `kind` is what gets constructed. They
// disagree on purpose: models 211-216 are helicopters the game types as cars, so
// they build as CAutomobile but belong under Air.
enum { VEH_CAT_LAND = 0, VEH_CAT_SEA, VEH_CAT_AIR, VEH_NUM_CATS };

static const char *const veh_cat_name[VEH_NUM_CATS] = {
  "Land", "Sea", "Air",
};

typedef struct {
  char label[28];
  int id;
  veh_kind kind;
  int cat;
} menu_veh_entry;

#define MENU_VEHICLE_MAX 160
static menu_veh_entry veh_list[MENU_VEHICLE_MAX];
static int vehicles_ready = 0;

static void veh_add(int id, veh_kind kind, int cat, const char *label) {
  if (vehicles_ready >= MENU_VEHICLE_MAX)
    return;
  menu_veh_entry *e = &veh_list[vehicles_ready++];
  e->id = id;
  e->kind = kind;
  e->cat = cat;
  snprintf(e->label, sizeof(e->label), "%s", label);
}


// Model ids only exist once the model info table has been built, which is long
// after patch_game, so this runs the first time the menu is opened in-game.
static void menu_resolve_vehicles(void) {
  static int done = 0;
  if (done || !num_model_infos)
    return;
  done = 1;

  // Every vehicle model the game has, in model order, each named by the game.
  const int total = *num_model_infos;
  int named = 0;
  for (int id = 0; id < total; id++) {
    const int kind = veh_classify(id);
    if (kind < 0)
      continue;
    if (is_in_cd_image && !is_in_cd_image(id))
      continue;
    if (veh_model_excluded(id)) {
      debugPrintf("MENU: model %d not listed (%s)\n", id, veh_exclude_reason(id));
      continue;
    }

    char label[28];
    // Our own label wins where we have one: the game's name for the RC
    // helicopter is no name at all, and nothing it says distinguishes the six
    // helicopters it calls cars.
    const char *extra = veh_extra_name(id);
    if (extra)
      snprintf(label, sizeof(label), "%s", extra);
    else if (vehicle_game_name(id, label, sizeof(label)))
      named++;
    else
      snprintf(label, sizeof(label), "%s %d", veh_kind_name[kind], id);

    // Air covers both the models the game types as helicopters and the ones it
    // types as cars but which fly anyway.
    int cat;
    if (kind == VEH_HELI || veh_model_is_air(id))
      cat = VEH_CAT_AIR;
    else if (kind == VEH_BOAT)
      cat = VEH_CAT_SEA;
    else
      cat = VEH_CAT_LAND;

    debugPrintf("MENU: model %3d  %-10s  %-5s  %s\n",
                id, veh_kind_name[kind], veh_cat_name[cat], label);
    veh_add(id, (veh_kind)kind, cat, label);
  }

  const int unnamed = vehicles_ready - named;

  int by_kind[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < vehicles_ready; i++)
    by_kind[veh_list[i].kind]++;

  debugPrintf("MENU: %d vehicles (%d named, %d unnamed) -- "
              "%d cars, %d bikes, %d boats, %d helicopters\n",
              vehicles_ready, named, unnamed,
              by_kind[VEH_AUTO], by_kind[VEH_BIKE],
              by_kind[VEH_BOAT], by_kind[VEH_HELI]);

  // Every unnamed entry, so the numbers can be matched up in game and turned
  // into real names in the table above.
  for (int i = named; i < vehicles_ready; i++)
    debugPrintf("MENU: unnamed %s, model %d\n",
                veh_kind_name[veh_list[i].kind], veh_list[i].id);

  // Vehicles said to exist in LCS but not normally reachable. The spawn list is
  // built by walking the model table and keeping whatever the Is*Model
  // predicates call a vehicle, so anything present *and* typed as a vehicle is
  // already in it -- but a model that exists with some other type would be
  // skipped silently. Asking by name settles both questions at once: the id if
  // the game knows the name at all, and the classification if it does.
  if (get_model_info) {
    // Asked of the running game rather than read out of anyone's data file.
    // The first two confirm that 211 and 212 are two different RC vehicles; the
    // rest are the ones said to exist but be unreachable.
    static const char *const probe[] = {
      "rcgoblin", "rcraider",
      "corpse", "mafiablood", "mini", "speakermav", "topfun", "deaddodo",
      "escape",
    };
    for (int i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++) {
      int id = -1;
      if (!get_model_info(probe[i], &id) || id < 0) {
        debugPrintf("MENU: extra model \"%s\" is not in this build\n", probe[i]);
        continue;
      }
      const int kind = veh_classify(id);
      debugPrintf("MENU: extra model \"%s\" = %d, %s, cd image %d\n", probe[i],
                  id, kind < 0 ? "NOT a spawnable vehicle" : veh_kind_name[kind],
                  is_in_cd_image ? is_in_cd_image(id) : -1);
    }
  }

  // One probe with a key the game itself uses (VehicleCheat passes "CHEAT1" to
  // CText::Get), so a zone-name miss can be told apart from the text system not
  // answering at all.
  char probe[40];
  debugPrintf("MENU: GXT probe CHEAT1 -> %s\n",
              gxt_lookup("CHEAT1", probe, sizeof(probe)) ? probe : "(miss)");
}

// ---- bodyguards ----
//
// CPopulation::AddPed takes its CVector by reference (RK7CVector in the mangled
// name), so unlike CPed::Teleport this one genuinely does want a pointer.
// ChooseGangOccupation picks a member model for a gang, so no ped model ids are
// hardcoded here either.
typedef void *(*add_ped_fn)(int ped_type, unsigned model, const float *pos,
                           int a, int b);
static add_ped_fn population_add_ped = NULL;
typedef int (*choose_gang_fn)(int gang);
static choose_gang_fn choose_gang_occupation = NULL;
// CPed::SetPlayerToFollow(int) is the follow-the-player primitive: it stores the
// player index at +473 and moves the ped into the follow state at +988.
typedef void (*set_player_to_follow_fn)(void *ped, int player);
// CPed::SetLeader(CPed*) -- one store, null-checked. Unlike
// SetPlayerToFollow, which dereferences a field a new ped does not have.
typedef void (*set_leader_fn)(void *ped, void *leader);
static set_leader_fn ped_set_leader = NULL;
typedef void (*give_weapon_fn)(void *ped, int weapon, unsigned ammo, int select);
static give_weapon_fn ped_give_weapon = NULL;
typedef void (*set_current_weapon_fn)(void *ped, int weapon);
static set_current_weapon_fn ped_set_current_weapon = NULL;

#define PEDTYPE_GANG1     7
#define BODYGUARD_GANG    0
#define BODYGUARD_COUNT   4
// eWeaponType is not exported as anything readable, so the weapon they carry is
// logged on spawn rather than asserted -- if this turns out to be the wrong one
// the log says which number to change.
#define BODYGUARD_WEAPON  17

// ---- the vehicle you are in ----
//
// Colours are a byte each at +576 and +577: CAutomobile::Render, CBike::Render
// and CBoat::Render all do
//
//   ldrb w2, [x19, #577] / ldrb w1, [x19, #576]
//   bl   CVehicleModelInfo::SetVehicleColour
//
// so they are per-vehicle and take effect on the next frame drawn.
//
// Velocity and mass come from CPhysical::ApplyMoveForce, which reads the mass at
// +240 and the velocity at +144 before adding force/mass to it. Everything here
// is per-vehicle on purpose: the handling record at +392 is **shared by every
// vehicle of that model**, so editing "speed" there would change every Banshee
// in the city, not the one you are sitting in.
#define VEH_COLOUR1   576
#define VEH_COLOUR2   577
#define VEH_MOVESPEED 144
#define VEH_MASS      240
#define VEH_NUM_COLOURS 128

typedef void *(*find_player_vehicle_fn)(void);
static find_player_vehicle_fn find_player_vehicle = NULL;
typedef void (*apply_move_force_fn)(void *phys, float x, float y, float z);
static apply_move_force_fn apply_move_force = NULL;

typedef enum {
  VEDIT_COLOUR1 = 0,
  VEDIT_COLOUR2,
  VEDIT_COLOUR_RANDOM,
  VEDIT_BOOST,
  VEDIT_UPRIGHT,
  VEDIT_STOP,
  VEDIT_NUM
} veh_edit_action;

static const char *const veh_edit_name[VEDIT_NUM] = {
  "Next colour 1",
  "Next colour 2",
  "Random colours",
  "Speed boost",
  "Flip upright",
  "Stop dead",
};


// ---- menu state ----
#define MENU_ROWS 8

typedef enum {
  MENU_ACT_CHEATS = 0,
  MENU_ACT_TELEPORT,
  MENU_ACT_VEHICLE,
  MENU_ACT_VEHEDIT,
  MENU_ACT_BODYGUARDS,
  MENU_ACT_CLEAR_WANTED,
  MENU_NUM_ACTIONS
} menu_action;

static const char *const menu_action_name[MENU_NUM_ACTIONS] = {
  "Cheats",
  "Teleport",
  "Spawn vehicle",
  "Edit vehicle",
  "Spawn bodyguards",
  "Clear wanted level",
};

// Top level = the actions, a header, then the toggles.
#define MENU_HDR_ROW   MENU_NUM_ACTIONS
#define MENU_TOP_ROWS  (MENU_NUM_ACTIONS + 1 + MENU_NUM_TOGGLES)

static int menu_open = 0;
static int menu_cursor = 0;
static int menu_dirty = 0;
static u64  menu_pad_prev = 0;

// The second level. One piece of state covers all three lists rather than a flag
// per list, so adding a fourth is a table entry instead of another branch.
typedef enum {
  SUB_NONE = 0,
  SUB_CHEATS,
  SUB_TELEPORT,
  SUB_VEHICLES,
  SUB_VEHEDIT
} menu_sub;

static menu_sub sub_kind = SUB_NONE;
static int sub_cursor = 0;

// Each list now opens on its categories and drills into one. `sub_cat` is -1
// while the categories are showing. The items of the chosen category are
// gathered into sub_index rather than filtered during drawing, so paging and
// selection stay plain array work.
#define MENU_SUB_MAX 192
static int sub_cat = -1;
static int cat_cursor = 0;
static int sub_index[MENU_SUB_MAX];
static int sub_index_count = 0;

// Teleport puts the map marker first, ahead of the three islands, because it is
// an action rather than a category -- picking it goes straight there.
#define TELEPORT_CAT_MARKER 0

static int sub_num_cats(void) {
  switch (sub_kind) {
    case SUB_CHEATS:   return CHEAT_NUM_CATS;
    case SUB_TELEPORT: return PLACE_NUM_CATS + 1;
    case SUB_VEHICLES: return VEH_NUM_CATS;
    case SUB_VEHEDIT:  return 1;   // flat list, entered straight away
    default:           return 0;
  }
}

static const char *sub_cat_label(int i) {
  switch (sub_kind) {
    case SUB_CHEATS:   return cheat_cat_name[i];
    case SUB_TELEPORT: return i == TELEPORT_CAT_MARKER ? "Map marker"
                                                       : place_cat_name[i - 1];
    case SUB_VEHICLES: return veh_cat_name[i];
    case SUB_VEHEDIT:  return "Vehicle";
    default:           return "";
  }
}

static int sub_total(void) {
  switch (sub_kind) {
    case SUB_CHEATS:   return cheats_ready;
    case SUB_TELEPORT: return MENU_NUM_PLACES;
    case SUB_VEHICLES: return vehicles_ready;
    case SUB_VEHEDIT:  return VEDIT_NUM;
    default:           return 0;
  }
}

// The category item `i` of the full list belongs to.
static int sub_item_cat(int i) {
  switch (sub_kind) {
    case SUB_CHEATS:   return menu_cheats[i].cat;
    case SUB_TELEPORT: return place_cat[i] + 1;   // 0 is the map marker
    case SUB_VEHICLES: return veh_list[i].cat;
    case SUB_VEHEDIT:  return 0;
    default:           return -1;
  }
}

static const char *sub_item_label(int i) {
  switch (sub_kind) {
    case SUB_CHEATS:   return menu_cheats[i].name;
    case SUB_TELEPORT: return place_label[i];
    case SUB_VEHICLES: return veh_list[i].label;
    case SUB_VEHEDIT:  return veh_edit_name[i];
    default:           return "";
  }
}

static const char *sub_title(void) {
  switch (sub_kind) {
    case SUB_CHEATS:   return "CHEATS";
    case SUB_TELEPORT: return "TELEPORT";
    case SUB_VEHICLES: return "SPAWN VEHICLE";
    case SUB_VEHEDIT:  return "EDIT VEHICLE";
    default:           return "";
  }
}

static void sub_build_index(int cat) {
  sub_index_count = 0;
  const int total = sub_total();
  for (int i = 0; i < total && sub_index_count < MENU_SUB_MAX; i++)
    if (sub_item_cat(i) == cat)
      sub_index[sub_index_count++] = i;
}

// How many rows the level currently showing has, and which cursor moves.
static int sub_count(void) {
  return sub_cat < 0 ? sub_num_cats() : sub_index_count;
}

static const char *sub_label(int i) {
  if (sub_cat < 0)
    return sub_cat_label(i);
  return sub_item_label(sub_index[i]);
}

// A short confirmation after an action, the way the game acknowledges its own
// cheat codes. Queued, because closing the menu wipes the help box on its way out.
static char toast[64];
static int toast_pending = 0;

// The last text we pushed, so we can tell when the game has overwritten it.
static uint16_t menu_wide[512];

static int menu_ready(void) {
  return hud_set_help_message && hud_help_forever && hud_help_message;
}

// CHud::m_HelpMessage is 0x200 bytes -- 256 UTF-16 characters, so 255 plus a
// terminator. Overrun it and the game stores a truncated copy, our comparison
// against what we pushed never matches again, and the menu re-pushes every
// frame: the help box restarts its animation 60 times a second, so nothing is
// ever legible and the cue machine-guns. That is what a sixth toggle did, by
// taking the top level from 246 characters to 268.
#define HUD_HELP_CHARS 256

static int menu_text_clobbered(void) {
  for (int i = 0; i < HUD_HELP_CHARS - 1; i++) {
    if (hud_help_message[i] != menu_wide[i])
      return 1;
    if (!menu_wide[i])
      return 0;
  }
  return 0;
}

// Clamped to what the game's buffer holds, so what we compare against is always
// what it actually stored.
static void menu_push(const char *text) {
  int i = 0;
  for (; text[i] && i < HUD_HELP_CHARS - 1; i++)
    menu_wide[i] = (uint16_t)(unsigned char)text[i];
  menu_wide[i] = 0;
  hud_set_help_message(menu_wide, 0, 0);
}

static void menu_render(void) {
  char line[512];
  int n = 0;

  if (sub_kind != SUB_NONE) {
    const int count = sub_count();
    const int cursor = sub_cat < 0 ? cat_cursor : sub_cursor;

    // The header carries the category once you are inside one, so the list
    // never leaves you wondering which of three islands you are looking at.
    if (sub_cat < 0)
      n += snprintf(line + n, sizeof(line) - n, "%s", sub_title());
    else
      n += snprintf(line + n, sizeof(line) - n, "%s - %s",
                    sub_title(), sub_cat_label(sub_cat));

    int first = cursor - MENU_ROWS / 2;
    if (first > count - MENU_ROWS)
      first = count - MENU_ROWS;
    if (first < 0)
      first = 0;

    for (int i = first; i < count && i < first + MENU_ROWS; i++) {
      n += snprintf(line + n, sizeof(line) - n, "~n~%c %s",
                    i == cursor ? '>' : ' ', sub_label(i));
      // Stop well short of what the help box holds; a row that would be
      // cut in half is a row that makes the text never match again.
      if (n >= HUD_HELP_CHARS - 40)
        break;
    }
  } else {
    n += snprintf(line + n, sizeof(line) - n, "LIBERTY MENU");

    // Windowed like the sub-lists. It used to draw all of itself, which was
    // fine until the rows outgrew the help box; scrolling means another toggle
    // costs nothing.
    int first = menu_cursor - MENU_ROWS / 2;
    if (first > MENU_TOP_ROWS - MENU_ROWS)
      first = MENU_TOP_ROWS - MENU_ROWS;
    if (first < 0)
      first = 0;

    for (int i = first; i < MENU_TOP_ROWS && i < first + MENU_ROWS; i++) {
      const char mark = i == menu_cursor ? '>' : ' ';
      if (i < MENU_NUM_ACTIONS) {
        n += snprintf(line + n, sizeof(line) - n, "~n~%c %s",
                      mark, menu_action_name[i]);
      } else if (i == MENU_HDR_ROW) {
        n += snprintf(line + n, sizeof(line) - n, "~n~  -- ALWAYS ON --");
      } else {
        const int t = i - MENU_HDR_ROW - 1;
        n += snprintf(line + n, sizeof(line) - n, "~n~%c %s  [%s]",
                      mark, menu_toggle_name[t],
                      menu_toggle_on[t] ? "ON" : "off");
      }
      // Stop well short of what the help box holds; a row that would be
      // cut in half is a row that makes the text never match again.
      if (n >= HUD_HELP_CHARS - 40)
        break;
    }
  }

  menu_push(line);
  *hud_help_forever = 1;
}

static void menu_close(void) {
  menu_open = 0;
  sub_kind = SUB_NONE;
  sub_cat = -1;
  g_menu_open = 0;
  *hud_help_forever = 0;
  menu_wide[0] = 0;
  static uint16_t empty[1] = { 0 };
  hud_set_help_message(empty, 0, 0);

  if (toast_pending) {
    toast_pending = 0;
    menu_push(toast);
  }
}

static void menu_sub_enter(menu_sub kind) {
  if (kind == SUB_TELEPORT)
    menu_label_places();
  sub_kind = kind;
  sub_cat = -1;          // categories first
  cat_cursor = 0;
  sub_cursor = 0;

  // Unless there is only one, in which case picking it is not a decision.
  if (sub_num_cats() == 1) {
    sub_cat = 0;
    sub_build_index(0);
  }
  menu_dirty = 1;
}

// ---- actions ----
static void menu_run_cheat(int idx) {
  if (idx < 0 || idx >= cheats_ready)
    return;

  debugPrintf("MENU: cheat %s\n", menu_cheats[idx].name);
  if (menu_cheats[idx].arg) {
    void (*fn1)(unsigned char) = (void (*)(unsigned char))menu_cheats[idx].fn;
    fn1(1);
  } else {
    menu_cheats[idx].fn();
  }

  snprintf(toast, sizeof(toast), "%s cheat activated", menu_cheats[idx].name);
  toast_pending = 1;
}

// Shared by the place list and the map marker. `snap_to_road` is what keeps the
// islands usable -- their zone centres are often buildings -- but a marker is a
// deliberate choice, so it is honoured exactly and only dropped to the ground.
static void menu_teleport_xy(float x, float y, int snap_to_road,
                             const char *what) {
  if (!find_ground_z || !find_player_ped || !ped_teleport) {
    snprintf(toast, sizeof(toast), "Teleport unavailable");
    toast_pending = 1;
    return;
  }

  if (!menu_pos_sane(x, y)) {
    debugPrintf("MENU: refusing to teleport to %s at %.1f, %.1f -- not a place\n",
                what, x, y);
    snprintf(toast, sizeof(toast), "That is not a valid location");
    toast_pending = 1;
    return;
  }

  void *ped = find_player_ped();
  if (!ped)
    return;

  float pos[3];
  const char *how = "ground";
  if (snap_to_road && road_near(x, y, 0.0f, pos)) {
    pos[2] += 1.5f;
    how = "road";
  } else {
    pos[0] = x;
    pos[1] = y;
    pos[2] = find_ground_z(x, y) + 1.5f;
  }

  debugPrintf("MENU: teleport to %s at %.1f, %.1f, %.1f (%s)\n",
              what, pos[0], pos[1], pos[2], how);
  ped_teleport(ped, pos);

  snprintf(toast, sizeof(toast), "Teleported to %s", what);
  toast_pending = 1;
}

static void menu_teleport_to(int idx) {
  if (idx < 0 || idx >= MENU_NUM_PLACES)
    return;
  menu_teleport_xy(place_x[idx], place_y[idx], 1, place_label[idx]);
}

static void menu_teleport_to_marker(void) {
  float x = 0.0f, y = 0.0f;
  if (!menu_find_marker(&x, &y)) {
    snprintf(toast, sizeof(toast), "Place a marker on the map first");
    toast_pending = 1;
    debugPrintf("MENU: teleport to marker, but no coordinate blip is set\n");
    return;
  }
  menu_teleport_xy(x, y, 0, "the marker");
}

// ---- watching a spawned vehicle ----
//
// If they still vanish, the useful question is *when*: a fixed delay points at
// something periodic, and an instant one points at the add itself. The pool
// handle is the safe way to ask -- CPools::GetVehicle checks the slot's flag
// byte against the handle, so a freed or reused slot answers NULL instead of
// handing back a dangling pointer to read.
typedef int (*get_vehicle_ref_fn)(void *veh);
typedef void *(*get_vehicle_fn)(int ref);
static get_vehicle_ref_fn pools_get_vehicle_ref = NULL;
static get_vehicle_fn pools_get_vehicle = NULL;

static int veh_track_ref = 0;
static u64 veh_track_t0 = 0;
static u64 veh_track_next_report = 0;
static char veh_track_label[28];

static void veh_track_begin(void *veh, const char *label) {
  if (!pools_get_vehicle_ref || !pools_get_vehicle)
    return;
  veh_track_ref = pools_get_vehicle_ref(veh);
  veh_track_t0 = armTicksToNs(armGetSystemTick());
  veh_track_next_report = 0;
  snprintf(veh_track_label, sizeof(veh_track_label), "%s", label);
  debugPrintf("MENU: watching %s, pool ref %d\n", veh_track_label, veh_track_ref);
}

static void veh_track_tick(void) {
  if (!veh_track_ref || !pools_get_vehicle)
    return;

  const u64 ms = (armTicksToNs(armGetSystemTick()) - veh_track_t0) / 1000000ull;

  void *veh = pools_get_vehicle(veh_track_ref);
  if (!veh) {
    debugPrintf("MENU: %s was removed from the pool after %llu ms\n",
                veh_track_label, (unsigned long long)ms);
    veh_track_ref = 0;
    return;
  }

  // Where it is, twice a second, while it still exists.
  //
  // Every spawn so far has been removed after a remarkably consistent 3.4-3.7
  // seconds. CCarCtrl::PossiblyRemoveVehicle only removes at a distance (its
  // thresholds are 190 and 70 units, and these sit five away), but
  // CWorld::RemoveFallenCars deletes anything below z = -100, and falling from
  // ground level to -100 takes about that long. If these numbers march
  // downwards, the vehicle is falling through the map and the fix belongs at
  // the spawn, not in whatever deletes it afterwards.
  if (ms >= veh_track_next_report) {
    veh_track_next_report = ms + 500;
    const float *p = (const float *)((uintptr_t)veh + VEH_POS);
    const uint64_t flags = *(const uint64_t *)((uintptr_t)veh + VEH_FLAGS);
    debugPrintf("MENU: %s at %llu ms: %.1f, %.1f, %.1f  status %u  level %u\n",
                veh_track_label, (unsigned long long)ms,
                p[0], p[1], p[2],
                (unsigned)((flags & VEH_STATUS_MASK) >> 4),
                *(const uint8_t *)((uintptr_t)veh + ENTITY_LEVEL));
  }
}

// ---- finding what SpawnInModel just made ----
//
// SpawnInModel returns nothing, so the vehicle it creates has to be found in
// the pool. CCarCtrl::RemoveDistantCars shows the layout: entries at +0, the
// per-slot flag bytes at +8, the count at +16, and a stride of 1968. A flag
// byte with its top bit set is a free slot.
//
// Snapshot which slots are taken, spawn, then look for the one that was not
// taken before.
#define VEHPOOL_ENTRIES 0
#define VEHPOOL_FLAGS   8
#define VEHPOOL_SIZE    16
#define VEHPOOL_STRIDE  1968
#define VEHPOOL_MAX     512

static void **ms_p_vehicle_pool = NULL;
static uint8_t veh_slot_taken[VEHPOOL_MAX];

static int veh_pool(uint8_t **entries, const int8_t **flags, int *size) {
  if (!ms_p_vehicle_pool)
    return 0;
  const uint8_t *pool = (const uint8_t *)*ms_p_vehicle_pool;
  if (!pool)
    return 0;

  *entries = *(uint8_t **)(pool + VEHPOOL_ENTRIES);
  *flags = *(const int8_t **)(pool + VEHPOOL_FLAGS);
  *size = *(const int *)(pool + VEHPOOL_SIZE);
  if (!*entries || !*flags || *size <= 0)
    return 0;
  if (*size > VEHPOOL_MAX)
    *size = VEHPOOL_MAX;
  return 1;
}

static void veh_pool_snapshot(void) {
  uint8_t *entries;
  const int8_t *flags;
  int size;
  memset(veh_slot_taken, 0, sizeof(veh_slot_taken));
  if (!veh_pool(&entries, &flags, &size))
    return;
  for (int i = 0; i < size; i++)
    veh_slot_taken[i] = flags[i] >= 0;
}

static void *veh_pool_find_new(void) {
  uint8_t *entries;
  const int8_t *flags;
  int size;
  if (!veh_pool(&entries, &flags, &size))
    return NULL;
  for (int i = 0; i < size; i++)
    if (flags[i] >= 0 && !veh_slot_taken[i])
      return entries + (size_t)i * VEHPOOL_STRIDE;
  return NULL;
}

// Dumps the fields a vehicle is made of, so ours can be held next to one the
// game made.
//
// Every theory so far has been wrong: it is not the status word, not the island
// byte, not the construction path (SpawnInModel is the game's own and its
// vehicles fall too), and not the matrix -- the rows logged as a clean rotation,
// (0.67 0.74 0)(-0.74 0.67 0)(0 0 1). What is left is that ours does not collide
// with the world while the traffic parked around it does, so the difference is
// in a field, and the fastest way to find a difference is to print both.
static void veh_dump(const char *what, const uint8_t *veh) {
  if (!veh) {
    debugPrintf("MENU: %-8s vehicle: none\n", what);
    return;
  }
  const float *pos = (const float *)(veh + VEH_POS);
  debugPrintf("MENU: %-8s model %d  flags88 %016llx  rwobj %p  col %p\n",
              what, (int)*(const int16_t *)(veh + 124),
              (unsigned long long)*(const uint64_t *)(veh + VEH_FLAGS),
              *(void *const *)(veh + 112),
              (void *)(model_info_for(*(const int16_t *)(veh + 124))
                           ? *(void *const *)(model_info_for(
                                                  *(const int16_t *)(veh + 124)) +
                                              MODELINFO_COL_MODEL)
                           : NULL));
  debugPrintf("MENU: %-8s w717 %08x  lvl390 %u  lvl126 %u  status784 %d  "
              "pos %.1f %.1f %.1f\n",
              what, *(const uint32_t *)(veh + 717), veh[390], veh[126],
              *(const int *)(veh + 784), pos[0], pos[1], pos[2]);
}

// Any vehicle that was already in the pool before we spawned ours -- i.e. one
// the game itself put there.
static const uint8_t *veh_pool_find_existing(const void *skip) {
  uint8_t *entries;
  const int8_t *flags;
  int size;
  if (!veh_pool(&entries, &flags, &size))
    return NULL;
  for (int i = 0; i < size; i++) {
    uint8_t *e = entries + (size_t)i * VEHPOOL_STRIDE;
    if (flags[i] < 0 || (const void *)e == skip)
      continue;
    // An in-use slot is not necessarily a vehicle: the first comparison came
    // back model 0, no clump, position 0,0,0. Insist on something real.
    if (*(const int16_t *)(e + 124) <= 0 || !*(void *const *)(e + 112))
      continue;
    return e;
  }
  return NULL;
}

// How much traffic exists at all.
//
// The interesting question is no longer "why does our vehicle fall" but "does
// anything else in this world stay up". The video shows a street with
// pedestrians on it and not one moving car, and the first attempt to compare
// against a game-made vehicle found none to compare with. If the pool is empty
// apart from ours, then vehicles falling through the map is the port's problem
// and not the menu's, and no amount of changing how the menu spawns them will
// help.
static void veh_pool_census(void) {
  uint8_t *entries;
  const int8_t *flags;
  int size;
  if (!veh_pool(&entries, &flags, &size)) {
    debugPrintf("MENU: vehicle pool unavailable\n");
    return;
  }

  int used = 0, real = 0;
  for (int i = 0; i < size; i++) {
    if (flags[i] < 0)
      continue;
    used++;
    const uint8_t *e = entries + (size_t)i * VEHPOOL_STRIDE;
    if (*(const int16_t *)(e + 124) > 0 && *(void *const *)(e + 112))
      real++;
  }
  debugPrintf("MENU: vehicle pool: %d slots, %d in use, %d with a model\n",
              size, used, real);
}

static void menu_spawn_vehicle(int idx) {
  if (idx < 0 || idx >= vehicles_ready)
    return;
  if (!spawn_in_model || !find_player_ped) {
    snprintf(toast, sizeof(toast), "Spawning unavailable");
    toast_pending = 1;
    return;
  }

  void *ped = find_player_ped();
  if (!ped)
    return;

  const menu_veh_entry *v = &veh_list[idx];

  // Just in front of the player. SpawnInModel drops it to the ground itself, so
  // only x and y matter -- z is handed in below the fallen-car threshold so that
  // its own FindGroundZForCoord branch is the one that runs.
  const float *ppos = (const float *)((uintptr_t)ped + PED_POS);
  const float *fwd = (const float *)((uintptr_t)ped + PED_FORWARD);
  float pos[3];
  pos[0] = ppos[0] + fwd[0] * SPAWN_AHEAD;
  pos[1] = ppos[1] + fwd[1] * SPAWN_AHEAD;
  pos[2] = -1000.0f;

  // VehicleCheat for anything it can handle, because it demonstrably works:
  // it is what TankCheat and TrashmasterCheat call, and the cars it spawned in
  // the earliest builds were solid enough to drive away.
  //
  // Replacing it was a mistake. It was replaced for two real reasons -- it
  // builds a boat as a CAutomobile and kills the game, and it drops the vehicle
  // on a path node up to 100 units off, which is the "spawned out of view"
  // complaint -- but the replacement lost the one thing that mattered, which is
  // that the vehicle sits on the ground instead of falling through it. Neither
  // hand-building nor SpawnInModel produced a vehicle that collides; both fall
  // straight down until CWorld::RemoveFallenCars takes them at -100.
  //
  // So: the proven path for everything except boats, and SpawnInModel only for
  // those, since VehicleCheat is the thing that crashes on them. The placement
  // is the game's choice again, which is a step back in convenience and a step
  // forward in existing.
  veh_pool_snapshot();
  const char *how = "VehicleCheat";
  if (v->kind == VEH_BOAT && spawn_in_model) {
    how = "SpawnInModel";
    spawn_in_model(v->id, pos);
  } else if (vehicle_cheat) {
    vehicle_cheat(v->id);
  } else {
    return;
  }

  void *veh = veh_pool_find_new();

  // VehicleCheat places the vehicle on the nearest path node within 100 units
  // and gives up entirely when there is none, which is why standing away from a
  // road produced "spawned" and nothing to show for it. SpawnInModel has no such
  // requirement -- it drops the vehicle at the coordinates it is handed.
  //
  // It gets used as the fallback rather than as the default because its
  // vehicles fell through the world when it was last tried. That turned out to
  // be the VehicleNames corruption rather than anything about SpawnInModel, but
  // "probably fine now" is not a reason to replace a path that works.
  if (!veh && spawn_in_model) {
    debugPrintf("MENU: VehicleCheat found no path node, falling back\n");
    how = "SpawnInModel (no road nearby)";
    spawn_in_model(v->id, pos);
    veh = veh_pool_find_new();
  }

  debugPrintf("MENU: spawn %s (model %d) via %s -> %g, %g, vehicle %p\n",
              v->label, v->id, how, (double)pos[0], (double)pos[1], veh);

  if (!veh) {
    snprintf(toast, sizeof(toast), "Could not place %s here", v->label);
    toast_pending = 1;
    return;
  }

  if (veh) {
    // Nothing is written to it. The matrix theory is dead -- the rows logged
    // last time were a clean rotation, (0.67 0.74 0)(-0.74 0.67 0)(0 0 1), and
    // it still fell. VehicleCheat sets up its own vehicle correctly, so the one
    // useful thing to do with it is look, not touch.
    const float *m = (const float *)((uintptr_t)veh + VEH_MATRIX);
    const float *p = (const float *)((uintptr_t)veh + VEH_POS);
    debugPrintf("MENU: %s rows (%.2f %.2f %.2f)(%.2f %.2f %.2f)(%.2f %.2f %.2f) "
                "pos %.1f %.1f %.1f\n",
                v->label, m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10],
                p[0], p[1], p[2]);
    veh_pool_census();
    veh_dump("ours", (const uint8_t *)veh);
    veh_dump("game's", veh_pool_find_existing(veh));
    veh_track_begin(veh, v->label);
  }

  snprintf(toast, sizeof(toast), "%s spawned", v->label);
  toast_pending = 1;
}

static void menu_spawn_bodyguards(void) {
  if (!population_add_ped || !choose_gang_occupation || !find_player_ped ||
      !ped_set_leader) {
    snprintf(toast, sizeof(toast), "Bodyguards unavailable");
    toast_pending = 1;
    return;
  }

  void *ped = find_player_ped();
  if (!ped)
    return;

  // Spread them around the player rather than stacking four peds on one spot.
  static const float dx[BODYGUARD_COUNT] = { 2.0f, -2.0f,  2.0f, -2.0f };
  static const float dy[BODYGUARD_COUNT] = { 2.0f,  2.0f, -2.0f, -2.0f };

  const float *ppos = (const float *)((uintptr_t)ped + PED_POS);
  int spawned = 0;

  for (int i = 0; i < BODYGUARD_COUNT; i++) {
    const int model = choose_gang_occupation(BODYGUARD_GANG);
    debugPrintf("MENU: bodyguard %d, gang %d -> ped model %d\n",
                i, BODYGUARD_GANG, model);
    if (model < 0) {
      debugPrintf("MENU: gang %d has no ped model\n", BODYGUARD_GANG);
      break;
    }

    // AddPed builds the ped straight away, so the model has to be resident
    // first. Requesting it was not enough on its own -- the game's own gang
    // spawner does the same request and then *checks*:
    //
    //   info = ms_modelInfoPtrs[model]
    //   x8 = info->vtable[6]; if (x8(info) == NULL) -> pick something else
    //
    // That virtual returns the model's clump, and a null one is what takes the
    // constructor down. CPopulation::AddPedInCar falls back when it sees a
    // null; we skip the guard instead, which is the honest thing to do when
    // there is nothing to fall back to.
    // Logged step by step. This has crashed three times and each log stopped at
    // the same place -- right after the model was chosen -- which narrows it to
    // "one of the next four calls" and no further. Guessing between them has not
    // worked, so each one now announces itself and the next log will name the
    // one that does not come back.
    if (is_in_cd_image && !is_in_cd_image(model)) {
      debugPrintf("MENU: ped model %d is not in the cd image, skipped\n", model);
      continue;
    }

    debugPrintf("MENU: ped %d: requesting model\n", model);
    if (request_model && load_all_models) {
      request_model(model, 1);
      debugPrintf("MENU: ped %d: requested, loading\n", model);
      load_all_models(0);
      debugPrintf("MENU: ped %d: loaded\n", model);
    }

    const uint8_t *pinfo = model_info_for(model);
    debugPrintf("MENU: ped %d: model info %p\n", model, (const void *)pinfo);
    if (!pinfo) {
      debugPrintf("MENU: ped model %d has no model info, skipped\n", model);
      continue;
    }
    if (!model_has_clump(model)) {
      debugPrintf("MENU: ped model %d did not load, skipping this bodyguard\n",
                  model);
      continue;
    }
    debugPrintf("MENU: ped %d: clump present\n", model);

    float pos[3];
    pos[0] = ppos[0] + dx[i];
    pos[1] = ppos[1] + dy[i];
    pos[2] = find_ground_z ? find_ground_z(pos[0], pos[1]) + 1.0f : ppos[2];

    debugPrintf("MENU: ped %d: AddPed(type %d) at %.1f, %.1f, %.1f\n",
                model, PEDTYPE_GANG1 + BODYGUARD_GANG, pos[0], pos[1], pos[2]);
    void *guard = population_add_ped(PEDTYPE_GANG1 + BODYGUARD_GANG,
                                     (unsigned)model, pos, 0, 0);
    debugPrintf("MENU: ped %d: AddPed returned %p\n", model, guard);
    if (!guard) {
      debugPrintf("MENU: AddPed(model %d) returned NULL\n", model);
      continue;
    }

    // NOT SetPlayerToFollow. That is what has been crashing all along, and the
    // instrumented log finally showed it: AddPed returns a valid ped, and the
    // next call dies. Its body is
    //
    //   ldr   x10, [x19, #1552]
    //   ldrsh w8,  [x10, #124]     ; and more: ldp q0,q1,[x10,#48] ...
    //
    // with no null check on x10. A ped fresh out of AddPed has nothing at
    // +1552, so it dereferences null immediately. The function expects a ped
    // the game has already set up, not a new one.
    //
    // CPed::SetLeader is the safe equivalent and is what the game's own
    // CPopulation::PlaceGangMembersInFormation uses to make gang members
    // follow: one store to +672, a null check, and a reference registration.
    if (ped_set_leader)
      ped_set_leader(guard, ped);

    // The weapon type was a guess and never got tested, because the crash came
    // first. Ask CWeaponInfo whether it is real before handing it over.
    const int wslot = weapon_slot_of(BODYGUARD_WEAPON);
    if (wslot >= 0 && ped_give_weapon) {
      ped_give_weapon(guard, BODYGUARD_WEAPON, AMMO_TOPUP, 1);
      if (ped_set_current_weapon)
        ped_set_current_weapon(guard, BODYGUARD_WEAPON);
    } else {
      debugPrintf("MENU: weapon type %d has no slot (%d), bodyguard unarmed\n",
                  BODYGUARD_WEAPON, wslot);
    }

    debugPrintf("MENU: bodyguard %d model %d weapon %d at %.1f, %.1f, %.1f\n",
                i, model, BODYGUARD_WEAPON, pos[0], pos[1], pos[2]);
    spawned++;
  }

  if (spawned)
    snprintf(toast, sizeof(toast), "%d bodyguards spawned", spawned);
  else
    snprintf(toast, sizeof(toast), "Could not spawn bodyguards");
  toast_pending = 1;
}

static void menu_vehicle_edit(int action) {
  if (!find_player_vehicle) {
    snprintf(toast, sizeof(toast), "Vehicle editing unavailable");
    toast_pending = 1;
    return;
  }

  uint8_t *veh = (uint8_t *)find_player_vehicle();
  if (!veh) {
    snprintf(toast, sizeof(toast), "Get in a vehicle first");
    toast_pending = 1;
    return;
  }

  float *vel = (float *)(veh + VEH_MOVESPEED);
  const float mass = *(const float *)(veh + VEH_MASS);
  float *rows = (float *)(veh + VEH_MATRIX);

  switch (action) {
    case VEDIT_COLOUR1:
      veh[VEH_COLOUR1] = (uint8_t)((veh[VEH_COLOUR1] + 1) % VEH_NUM_COLOURS);
      snprintf(toast, sizeof(toast), "Colour 1: %u", veh[VEH_COLOUR1]);
      break;

    case VEDIT_COLOUR2:
      veh[VEH_COLOUR2] = (uint8_t)((veh[VEH_COLOUR2] + 1) % VEH_NUM_COLOURS);
      snprintf(toast, sizeof(toast), "Colour 2: %u", veh[VEH_COLOUR2]);
      break;

    case VEDIT_COLOUR_RANDOM: {
      // The frame counter is as good a source of noise as anything here, and it
      // avoids dragging in the game's RNG.
      static unsigned seed = 12345;
      seed = seed * 1103515245u + 12345u;
      veh[VEH_COLOUR1] = (uint8_t)((seed >> 16) % VEH_NUM_COLOURS);
      veh[VEH_COLOUR2] = (uint8_t)((seed >> 8) % VEH_NUM_COLOURS);
      snprintf(toast, sizeof(toast), "Colours: %u / %u", veh[VEH_COLOUR1],
               veh[VEH_COLOUR2]);
      break;
    }

    case VEDIT_BOOST: {
      // Along the vehicle's own forward vector, through the game's own force
      // routine so the result is a push rather than a teleport. ApplyMoveForce
      // divides by mass, so multiplying by it asks for an acceleration.
      const float *fwd = rows + 4;   // row 1, the forward row
      if (apply_move_force)
        apply_move_force(veh, fwd[0] * mass * 0.35f, fwd[1] * mass * 0.35f,
                         fwd[2] * mass * 0.35f);
      snprintf(toast, sizeof(toast), "Boost");
      break;
    }

    case VEDIT_UPRIGHT: {
      // Rebuild the rotation from the heading it already has, which is what
      // CPlaceable::SetHeading does: row0 = (cos, sin, 0), row1 = (-sin, cos, 0).
      float fx = rows[4], fy = rows[5];
      const float len = __builtin_sqrtf(fx * fx + fy * fy);
      if (len < 0.0001f) {
        fx = 0.0f; fy = 1.0f;
      } else {
        fx /= len; fy /= len;
      }
      rows[0] = fy;  rows[1] = -fx; rows[2] = 0.0f;
      rows[4] = fx;  rows[5] = fy;  rows[6] = 0.0f;
      rows[8] = 0.0f; rows[9] = 0.0f; rows[10] = 1.0f;
      vel[2] = 0.2f;   // a nudge upward so it does not resolve into the ground
      snprintf(toast, sizeof(toast), "Flipped upright");
      break;
    }

    case VEDIT_STOP:
      vel[0] = vel[1] = vel[2] = 0.0f;
      snprintf(toast, sizeof(toast), "Stopped");
      break;

    default:
      return;
  }

  debugPrintf("MENU: vehicle edit %d -> colours %u/%u, vel %g %g %g\n", action,
              veh[VEH_COLOUR1], veh[VEH_COLOUR2], (double)vel[0], (double)vel[1],
              (double)vel[2]);
  toast_pending = 1;
}

static void menu_clear_wanted(void) {
  if (!cheat_wanted_level || !find_player_ped)
    return;
  void *ped = find_player_ped();
  if (!ped)
    return;
  cheat_wanted_level((void *)((uintptr_t)ped + PED_WANTED_OFFSET), 0);
  snprintf(toast, sizeof(toast), "Wanted level cleared");
  toast_pending = 1;
}

static void menu_activate(void) {
  if (sub_kind != SUB_NONE) {
    // Choosing a category drills in rather than doing anything -- except the
    // map marker, which is an action sitting among the islands.
    if (sub_cat < 0) {
      if (sub_kind == SUB_TELEPORT && cat_cursor == TELEPORT_CAT_MARKER) {
        menu_teleport_to_marker();
        menu_close();
        return;
      }
      sub_cat = cat_cursor;
      sub_build_index(sub_cat);
      sub_cursor = 0;
      menu_dirty = 1;
      return;
    }

    if (sub_cursor < 0 || sub_cursor >= sub_index_count)
      return;
    const int item = sub_index[sub_cursor];

    switch (sub_kind) {
      case SUB_CHEATS:   menu_run_cheat(item);     break;
      case SUB_TELEPORT: menu_teleport_to(item);   break;
      case SUB_VEHICLES: menu_spawn_vehicle(item); break;
      case SUB_VEHEDIT:  menu_vehicle_edit(item);  break;
      default: break;
    }
    menu_close();
    return;
  }

  if (menu_cursor > MENU_HDR_ROW) {
    const int t = menu_cursor - MENU_HDR_ROW - 1;
    if (t >= 0 && t < MENU_NUM_TOGGLES) {
      menu_toggle_on[t] = !menu_toggle_on[t];
      debugPrintf("MENU: %s -> %s\n", menu_toggle_name[t],
                  menu_toggle_on[t] ? "ON" : "off");
      menu_dirty = 1;
    }
    return;
  }

  switch (menu_cursor) {
    case MENU_ACT_CHEATS:
      if (cheats_ready)
        menu_sub_enter(SUB_CHEATS);
      break;
    case MENU_ACT_TELEPORT:
      menu_sub_enter(SUB_TELEPORT);
      break;
    case MENU_ACT_VEHICLE:
      if (vehicles_ready)
        menu_sub_enter(SUB_VEHICLES);
      break;
    case MENU_ACT_VEHEDIT:
      menu_sub_enter(SUB_VEHEDIT);
      break;
    case MENU_ACT_BODYGUARDS:
      menu_spawn_bodyguards();
      menu_close();
      break;
    case MENU_ACT_CLEAR_WANTED:
      menu_clear_wanted();
      menu_close();
      break;
    default:
      break;
  }
}

// ---- per-frame ----
void menu_tick(int in_game) {
  if (!menu_ready())
    return;

  // The help box belongs to the in-game HUD; opening the menu on the front end
  // or during load would draw through a text system that is not up yet.
  if (!in_game) {
    if (menu_open)
      menu_close();
    menu_pad_prev = g_menu_pad_down;
    return;
  }

  menu_apply_toggles();
  veh_track_tick();

  const u64 down = g_menu_pad_down;
  const u64 pressed = down & ~menu_pad_prev;
  menu_pad_prev = down;

  // Input suppression must never outlive the menu.
  if (!menu_open && g_menu_open)
    g_menu_open = 0;

  if (!menu_open) {
    if (pressed & HidNpadButton_Minus) {
      menu_resolve_vehicles();
      menu_open = 1;
      g_menu_open = 1;
      menu_cursor = 0;
      sub_kind = SUB_NONE;
      sub_cat = -1;
      menu_dirty = 1;
    }
    return;
  }

  // B unwinds one level at a time: items to categories, categories to the top,
  // top to closed.
  if (pressed & (HidNpadButton_B | HidNpadButton_Minus)) {
    if (sub_cat >= 0) {
      sub_cat = -1;
      menu_dirty = 1;
    } else if (sub_kind != SUB_NONE) {
      sub_kind = SUB_NONE;
      menu_dirty = 1;
    } else {
      menu_close();
    }
    return;
  }

  const int count = sub_kind != SUB_NONE ? sub_count() : MENU_TOP_ROWS;
  int *cursor = sub_kind == SUB_NONE ? &menu_cursor
                                     : (sub_cat < 0 ? &cat_cursor : &sub_cursor);
  if (count <= 0)
    return;

  // The section header is a label, so the cursor steps over it.
  if (pressed & HidNpadButton_Up) {
    do {
      if (--*cursor < 0)
        *cursor = count - 1;
    } while (sub_kind == SUB_NONE && *cursor == MENU_HDR_ROW);
    menu_dirty = 1;
  }
  if (pressed & HidNpadButton_Down) {
    do {
      if (++*cursor >= count)
        *cursor = 0;
    } while (sub_kind == SUB_NONE && *cursor == MENU_HDR_ROW);
    menu_dirty = 1;
  }
  if (pressed & HidNpadButton_A)
    menu_activate();

  // Re-push on a real change, or when a game tip has taken the help box from us.
  if (menu_open && (menu_dirty || menu_text_clobbered())) {
    menu_render();
    menu_dirty = 0;
  }
}

// ---- setup ----
static uintptr_t need_sym(const char *sym) {
  const uintptr_t a = so_try_find_addr_rx(&game_mod, sym);
  if (!a)
    debugPrintf("MENU: %s not found\n", sym);
  return a;
}

void menu_init(void) {
  // Cached for use at runtime, so they must come from load_virtbase
  // (so_try_find_addr_rx): after so_finalize remaps the image the load_base
  // alias is gone. See the note in patch_game.
  hud_set_help_message = (set_help_msg_fn)need_sym("_ZN4CHud14SetHelpMessageEPtbb");
  hud_help_forever = (uint8_t *)need_sym("_ZN4CHud27m_HelpMessageDisplayForeverE");
  hud_help_message = (uint16_t *)need_sym("_ZN4CHud13m_HelpMessageE");

  ped_set_ammo = (set_ammo_fn)need_sym("_ZN4CPed7SetAmmoE11eWeaponTypej");
  get_weapon_info =
      (get_weapon_info_fn)need_sym("_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponType");
  cheat_wanted_level = (cheat_wanted_fn)need_sym("_ZN7CWanted16CheatWantedLevelEi");
  find_ground_z = (find_ground_z_fn)need_sym("_ZN6CWorld19FindGroundZForCoordEff");
  find_player_ped = (find_player_ped_fn)need_sym("_Z13FindPlayerPedv");
  find_player_vehicle =
      (find_player_vehicle_fn)need_sym("_Z17FindPlayerVehiclev");
  apply_move_force =
      (apply_move_force_fn)need_sym("_ZN9CPhysical14ApplyMoveForceEfff");
  ped_teleport = (ped_teleport_fn)need_sym("_ZN4CPed8TeleportE7CVector");

  ctext_instance = (void **)need_sym("_ZN5CText10msInstanceE");

  gp_the_zones = (void **)need_sym("gpTheZones");
  gp_the_paths = (void **)need_sym("gpThePaths");
  level_from_position =
      (level_from_pos_fn)need_sym("_ZN9CTheZones20GetLevelFromPositionEPK7CVector");
  radar_trace = (uint8_t *)need_sym("_ZN6CRadar13ms_RadarTraceE");
  fly_height_limit = (float *)need_sym("_ZN8CVehicle17rcHeliHeightLimitE");
  menu_target_on = (uint8_t *)need_sym("_ZN12CMenuManager12m_TargetIsOnE");
  menu_target_pos = (float *)need_sym("_ZN12CMenuManager12m_fTargetPosE");
  radar_map_waypoint = (float *)need_sym("_ZN6CRadar11MapWayPointE");
  gp_radar_map = (void **)need_sym("GRadarMap");
  menu_target_blip_index =
      (int *)need_sym("_ZN12CMenuManager17m_TargetBlipIndexE");
  fly_raise_hard_cap();
  find_node_closest =
      (find_node_fn)need_sym("_ZN9CPathFind22FindNodeClosestToCoorsE7CVectorhfbbbb");
  find_zone_by_label =
      (find_zone_label_fn)need_sym("_ZN9CTheZones29FindZoneByLabelAndReturnIndexEPc9eZoneType");
  get_nav_zone = (get_zone_fn)need_sym("_ZN9CTheZones17GetNavigationZoneEt");
  get_info_zone = (get_zone_fn)need_sym("_ZN9CTheZones11GetInfoZoneEt");
  get_map_zone = (get_zone_fn)need_sym("_ZN9CTheZones10GetMapZoneEt");
  zone_translated_name = (zone_name_fn)need_sym("_ZN5CZone17GetTranslatedNameEv");
  text_get = (text_get_fn)need_sym("_ZN5CText3GetEPKc");
  ms_model_info_ptrs = (void ***)need_sym("_ZN10CModelInfo16ms_modelInfoPtrsE");

  spawn_in_model = (spawn_in_model_fn)need_sym("_Z12SpawnInModeli7CVector");
  vehicle_cheat = (vehicle_cheat_fn)need_sym("_Z12VehicleCheati");
  ms_p_vehicle_pool = (void **)need_sym("_ZN6CPools15ms_pVehiclePoolE");
  pools_get_vehicle_ref =
      (get_vehicle_ref_fn)need_sym("_ZN6CPools13GetVehicleRefEP8CVehicle");
  pools_get_vehicle = (get_vehicle_fn)need_sym("_ZN6CPools10GetVehicleEi");
  get_model_info =
      (get_model_info_fn)need_sym("_ZN10CModelInfo12GetModelInfoEPKcPi");
  request_model = (request_model_fn)need_sym("_ZN10CStreaming12RequestModelEii");
  load_all_models = (load_all_models_fn)need_sym("_ZN10CStreaming22LoadAllRequestedModelsEb");
  is_car_model = (is_model_fn)need_sym("_ZN10CModelInfo10IsCarModelEi");
  is_bike_model = (is_model_fn)need_sym("_ZN10CModelInfo11IsBikeModelEi");
  is_boat_model = (is_model_fn)need_sym("_ZN10CModelInfo11IsBoatModelEi");
  is_heli_model = (is_model_fn)need_sym("_ZN10CModelInfo11IsHeliModelEi");
  num_model_infos = (const int *)need_sym("_ZN10CModelInfo15msNumModelInfosE");
  is_in_cd_image = (is_model_fn)need_sym("_ZN10CStreaming17IsObjectInCdImageEi");

  population_add_ped =
      (add_ped_fn)need_sym("_ZN11CPopulation6AddPedE8ePedTypejRK7CVectorib");
  choose_gang_occupation =
      (choose_gang_fn)need_sym("_ZN11CPopulation20ChooseGangOccupationEi");
  ped_set_leader = (set_leader_fn)need_sym("_ZN4CPed9SetLeaderEPS_");
  ped_give_weapon = (give_weapon_fn)need_sym("_ZN4CPed10GiveWeaponE11eWeaponTypejb");
  ped_set_current_weapon =
      (set_current_weapon_fn)need_sym("_ZN4CPed16SetCurrentWeaponE11eWeaponType");

  int kept = 0;
  for (int i = 0; i < MENU_NUM_CHEATS; i++) {
    const uintptr_t a = so_try_find_addr_rx(&game_mod, menu_cheats[i].sym);
    if (!a) {
      debugPrintf("MENU: cheat \"%s\" not exported, dropped\n", menu_cheats[i].name);
      continue;
    }
    menu_cheats[kept] = menu_cheats[i];
    menu_cheats[kept].fn = (void (*)(void))a;
    kept++;
  }
  cheats_ready = kept;

  debugPrintf("MENU: Liberty Menu ready (%d/%d cheats)\n", kept, MENU_NUM_CHEATS);
}
