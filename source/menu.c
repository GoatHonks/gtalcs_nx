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
} menu_cheat;

static menu_cheat menu_cheats[] = {
  { "Weapons 1",        "_Z12WeaponCheat1v",           NULL, 0 },
  { "Weapons 2",        "_Z12WeaponCheat2v",           NULL, 0 },
  { "Weapons 3",        "_Z12WeaponCheat3v",           NULL, 0 },
  // HealthCheat is the one cheat that takes a parameter, and it gates the game's
  // own on-screen confirmation (tst w0,#0xff / b.eq past the CText work), so
  // pass 1 to get the native message rather than a silent top-up.
  { "Health",           "_Z11HealthCheath",            NULL, 1 },
  { "Armour",           "_Z11ArmourCheatv",            NULL, 0 },
  { "Money",            "_Z10MoneyCheatv",             NULL, 0 },
  { "Wanted up",        "_Z18WantedLevelUpCheatv",     NULL, 0 },
  { "Wanted down",      "_Z20WantedLevelDownCheatv",   NULL, 0 },
  { "Sunny",            "_Z17SunnyWeatherCheatv",      NULL, 0 },
  { "Extra sunny",      "_Z22ExtraSunnyWeatherCheatv", NULL, 0 },
  { "Cloudy",           "_Z18CloudyWeatherCheatv",     NULL, 0 },
  { "Rainy",            "_Z17RainyWeatherCheatv",      NULL, 0 },
  { "Foggy",            "_Z17FoggyWeatherCheatv",      NULL, 0 },
  { "Faster time",      "_Z13FastTimeCheatv",          NULL, 0 },
  { "Slower time",      "_Z13SlowTimeCheatv",          NULL, 0 },
  { "Faster weather",   "_Z16FastWeatherCheatv",       NULL, 0 },
  // TankCheat is not "spawn a tank". It walks a counter over the whole vehicle
  // model range (130..216) and hands whatever it lands on to VehicleCheat, so it
  // spawns a different vehicle every time -- which is exactly the random cars
  // that showed up. Spawn vehicle below is the same VehicleCheat call with the
  // model chosen deliberately; this entry stays, honestly named, because the
  // cycling is the game's own cheat.
  { "Random vehicle",   "_Z9TankCheatv",               NULL, 0 },
  { "Trashmaster",      "_Z16TrashmasterCheatv",       NULL, 0 },
  { "Chromed cars",     "_Z14GlassCarsCheatv",         NULL, 0 },
  { "Black cars",       "_Z14BlackCarsCheatv",         NULL, 0 },
  { "Pink cars",        "_Z13PinkCarsCheatv",          NULL, 0 },
  { "Mini wheels",      "_Z15BikeWheelsCheatv",        NULL, 0 },
  { "Big heads",        "_Z13BigHeadsCheatv",          NULL, 0 },
  { "Blow up cars",     "_Z15BlowUpCarsCheatv",        NULL, 0 },
  { "Mad drivers",      "_Z12MadCarsCheatv",           NULL, 0 },
  { "Peds riot",        "_Z11MayhemCheatv",            NULL, 0 },
  { "Peds attack you",  "_Z27EverybodyAttacksPlayerCheatv", NULL, 0 },
  { "Peds have weapons","_Z21DoChicksWithGunsCheatv",  NULL, 0 },
};
#define MENU_NUM_CHEATS ((int)(sizeof(menu_cheats) / sizeof(menu_cheats[0])))

static int cheats_ready = 0;

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
typedef void (*set_ammo_fn)(void *ped, int weapon_type, unsigned count);
static set_ammo_fn ped_set_ammo = NULL;
#define WEAPON_TYPE_MAX 36
#define AMMO_TOPUP      9999

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
  MENU_NUM_TOGGLES
} menu_toggle;

static const char *const menu_toggle_name[MENU_NUM_TOGGLES] = {
  "Never tired",
  "Regen health & armour",
  "Invincible",
  "Unlimited ammo",
  "Never wanted",
};

static int menu_toggle_on[MENU_NUM_TOGGLES];

// Runs every frame while the game is live, menu open or not.
static void menu_apply_toggles(void) {
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
static char place_label[MENU_NUM_PLACES][40];
static float place_x[MENU_NUM_PLACES];
static float place_y[MENU_NUM_PLACES];
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
#define VEH_MATRIX     16    // three 16-byte rotation rows, +16..+63
#define VEH_MATRIX_LEN 48
#define VEH_POS        64    // x,y at +64/+68 and z at +72, per VehicleCheat's
                             // str d1,[x20,#64] / str s0,[x20,#72]
#define VEH_STATUS     784   // VehicleCheat's str w9(#1),[x20,#784]

// The entity flags word, and the one write that was missing. VehicleCheat does
//
//   ldr x10,[x20,#88] / and x10,x10,#0xfffffffffffffe0f / orr x8,x10,#0x40
//
// which clears the five-bit field at bit 4 and sets it to 4 -- STATUS_ABANDONED,
// the status a parked car is meant to have. 82 other places in the binary write
// this same field, so it is not incidental. Leaving it at whatever the
// constructor produced is why spawned vehicles stopped appearing: they were
// built and added to the world, but with a status the engine does not process
// or draw.
#define VEH_FLAGS            88
#define VEH_STATUS_MASK      0x1f0ULL
#define VEH_STATUS_ABANDONED 0x40ULL

// The player's forward vector is the second matrix row: CPlaceable::SetHeading
// builds row0 = (cos, sin, 0) and row1 = (-sin, cos, 0), and GTA faces +y.
#define PED_FORWARD    (VEH_MATRIX + 16)
#define SPAWN_AHEAD    5.0f

// Allocation sizes come from the callers of each constructor: CVehicle::operator
// new is handed 0x7b0 before CAutomobile, 0x6a0 before CBike and 0x610 before
// CBoat.
#define VEH_SIZE_AUTO 0x7b0
#define VEH_SIZE_BIKE 0x6a0
#define VEH_SIZE_BOAT 0x610
#define VEH_SIZE_HELI 0x490

// Planes and trains are deliberately absent. `CPlane::CPlane` exists but nothing
// in this build ever calls it -- LCS has no flyable plane -- so its allocation
// size cannot be read off a caller the way the others can, and guessing the size
// of a heap allocation is not worth a spawnable Dodo. Trains need rails.
typedef enum { VEH_AUTO = 0, VEH_BIKE, VEH_BOAT, VEH_HELI } veh_kind;

static const char *const veh_kind_name[] = { "Car", "Bike", "Boat", "Helicopter" };

typedef void *(*vehicle_new_fn)(size_t size);
typedef void (*vehicle_ctor_fn)(void *self, int model, unsigned char created_by);
typedef void (*world_add_fn)(void *entity);
typedef void (*request_model_fn)(int model, int flags);
typedef void (*load_all_models_fn)(int prio);
typedef char (*is_model_fn)(int model);

static vehicle_new_fn vehicle_new = NULL;
static vehicle_ctor_fn automobile_ctor = NULL;
static vehicle_ctor_fn bike_ctor = NULL;
static vehicle_ctor_fn boat_ctor = NULL;
static vehicle_ctor_fn heli_ctor = NULL;
static world_add_fn world_add = NULL;
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
static const struct { int id; const char *name; } veh_extra_names[] = {
  { 211, "Small heli (no tex)" },   // untextured; looks unfinished
  { 212, "Small heli 2 (no tex)" }, // identical to 211
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
typedef struct {
  char label[28];
  int id;
  veh_kind kind;
} menu_veh_entry;

#define MENU_VEHICLE_MAX 160
static menu_veh_entry veh_list[MENU_VEHICLE_MAX];
static int vehicles_ready = 0;

static void veh_add(int id, veh_kind kind, const char *label) {
  if (vehicles_ready >= MENU_VEHICLE_MAX)
    return;
  menu_veh_entry *e = &veh_list[vehicles_ready++];
  e->id = id;
  e->kind = kind;
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

    char label[28];
    const char *extra = veh_extra_name(id);
    if (vehicle_game_name(id, label, sizeof(label)))
      named++;
    else if (extra)
      snprintf(label, sizeof(label), "%s", extra);
    else
      snprintf(label, sizeof(label), "%s %d", veh_kind_name[kind], id);

    debugPrintf("MENU: model %3d  %-10s  %s\n", id, veh_kind_name[kind], label);
    veh_add(id, (veh_kind)kind, label);
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
static set_player_to_follow_fn ped_set_player_to_follow = NULL;
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

// ---- menu state ----
#define MENU_ROWS 8

typedef enum {
  MENU_ACT_CHEATS = 0,
  MENU_ACT_TELEPORT,
  MENU_ACT_VEHICLE,
  MENU_ACT_BODYGUARDS,
  MENU_ACT_CLEAR_WANTED,
  MENU_NUM_ACTIONS
} menu_action;

static const char *const menu_action_name[MENU_NUM_ACTIONS] = {
  "Cheats",
  "Teleport",
  "Spawn vehicle",
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
  SUB_VEHICLES
} menu_sub;

static menu_sub sub_kind = SUB_NONE;
static int sub_cursor = 0;

static int sub_count(void) {
  switch (sub_kind) {
    case SUB_CHEATS:   return cheats_ready;
    case SUB_TELEPORT: return MENU_NUM_PLACES;
    case SUB_VEHICLES: return vehicles_ready;
    default:           return 0;
  }
}

static const char *sub_title(void) {
  switch (sub_kind) {
    case SUB_CHEATS:   return "CHEATS";
    case SUB_TELEPORT: return "TELEPORT";
    case SUB_VEHICLES: return "SPAWN VEHICLE";
    default:           return "";
  }
}

static const char *sub_label(int i) {
  switch (sub_kind) {
    case SUB_CHEATS:   return menu_cheats[i].name;
    case SUB_TELEPORT: return place_label[i];
    case SUB_VEHICLES: return veh_list[i].label;
    default:           return "";
  }
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

static int menu_text_clobbered(void) {
  for (int i = 0; i < 256; i++) {
    if (hud_help_message[i] != menu_wide[i])
      return 1;
    if (!menu_wide[i])
      return 0;
  }
  return 0;
}

static void menu_push(const char *text) {
  int i = 0;
  for (; text[i] && i < (int)(sizeof(menu_wide) / sizeof(menu_wide[0])) - 1; i++)
    menu_wide[i] = (uint16_t)(unsigned char)text[i];
  menu_wide[i] = 0;
  hud_set_help_message(menu_wide, 0, 0);
}

static void menu_render(void) {
  char line[512];
  int n = 0;

  if (sub_kind != SUB_NONE) {
    const int count = sub_count();
    n += snprintf(line + n, sizeof(line) - n, "%s", sub_title());

    int first = sub_cursor - MENU_ROWS / 2;
    if (first > count - MENU_ROWS)
      first = count - MENU_ROWS;
    if (first < 0)
      first = 0;

    for (int i = first; i < count && i < first + MENU_ROWS; i++) {
      n += snprintf(line + n, sizeof(line) - n, "~n~%c %s",
                    i == sub_cursor ? '>' : ' ', sub_label(i));
      if (n >= (int)sizeof(line) - 48)
        break;
    }
  } else {
    n += snprintf(line + n, sizeof(line) - n, "LIBERTY MENU");

    for (int i = 0; i < MENU_NUM_ACTIONS; i++) {
      n += snprintf(line + n, sizeof(line) - n, "~n~%c %s",
                    i == menu_cursor ? '>' : ' ', menu_action_name[i]);
    }

    n += snprintf(line + n, sizeof(line) - n, "~n~  -- ALWAYS ON --");

    for (int i = 0; i < MENU_NUM_TOGGLES; i++) {
      const int row = MENU_HDR_ROW + 1 + i;
      n += snprintf(line + n, sizeof(line) - n, "~n~%c %s  [%s]",
                    row == menu_cursor ? '>' : ' ', menu_toggle_name[i],
                    menu_toggle_on[i] ? "ON" : "off");
    }
  }

  menu_push(line);
  *hud_help_forever = 1;
}

static void menu_close(void) {
  menu_open = 0;
  sub_kind = SUB_NONE;
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
  sub_cursor = 0;
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

static void menu_teleport_to(int idx) {
  if (idx < 0 || idx >= MENU_NUM_PLACES)
    return;
  if (!find_ground_z || !find_player_ped || !ped_teleport) {
    snprintf(toast, sizeof(toast), "Teleport unavailable");
    toast_pending = 1;
    return;
  }

  void *ped = find_player_ped();
  if (!ped)
    return;

  // Aim for the nearest road rather than the zone's exact centre, which can be
  // the middle of a building. Falling back on the ground height keeps the entry
  // usable for anywhere the path network does not reach.
  float pos[3];
  const char *how;
  if (road_near(place_x[idx], place_y[idx], 0.0f, pos)) {
    pos[2] += 1.5f;
    how = "road";
  } else {
    pos[0] = place_x[idx];
    pos[1] = place_y[idx];
    pos[2] = find_ground_z(pos[0], pos[1]) + 1.5f;
    how = "ground";
  }

  debugPrintf("MENU: teleport to %s at %.1f, %.1f, %.1f (%s)\n",
              place_label[idx], pos[0], pos[1], pos[2], how);
  ped_teleport(ped, pos);

  snprintf(toast, sizeof(toast), "Teleported to %s", place_label[idx]);
  toast_pending = 1;
}

static void menu_spawn_vehicle(int idx) {
  if (idx < 0 || idx >= vehicles_ready)
    return;
  if (!vehicle_new || !world_add || !request_model || !load_all_models ||
      !find_player_ped || !automobile_ctor) {
    snprintf(toast, sizeof(toast), "Spawning unavailable");
    toast_pending = 1;
    return;
  }

  void *ped = find_player_ped();
  if (!ped)
    return;

  const menu_veh_entry *v = &veh_list[idx];

  // Blocking load, the same pair VehicleCheat uses.
  request_model(v->id, 1);
  load_all_models(0);

  size_t size;
  vehicle_ctor_fn ctor;
  switch (v->kind) {
    case VEH_BIKE: size = VEH_SIZE_BIKE; ctor = bike_ctor; break;
    case VEH_BOAT: size = VEH_SIZE_BOAT; ctor = boat_ctor; break;
    case VEH_HELI: size = VEH_SIZE_HELI; ctor = heli_ctor; break;
    default:       size = VEH_SIZE_AUTO; ctor = automobile_ctor; break;
  }
  if (!ctor)
    return;

  void *veh = vehicle_new(size);
  if (!veh) {
    debugPrintf("MENU: no room for %s\n", v->label);
    return;
  }
  ctor(veh, v->id, 1);   // 1 = created by the game rather than by a mission

  // Face it the way the player is facing by copying their three rotation rows,
  // which is both simpler and safer than building a matrix from a heading.
  memcpy((char *)veh + VEH_MATRIX, (const char *)ped + VEH_MATRIX, VEH_MATRIX_LEN);

  // Just in front of the player rather than on a path node up the road.
  const float *ppos = (const float *)((uintptr_t)ped + PED_POS);
  const float *fwd = (const float *)((uintptr_t)ped + PED_FORWARD);
  float *vpos = (float *)((uintptr_t)veh + VEH_POS);
  vpos[0] = ppos[0] + fwd[0] * SPAWN_AHEAD;
  vpos[1] = ppos[1] + fwd[1] * SPAWN_AHEAD;
  vpos[2] = find_ground_z ? find_ground_z(vpos[0], vpos[1]) + 1.0f : ppos[2];

  uint64_t *flags = (uint64_t *)((uintptr_t)veh + VEH_FLAGS);
  *flags = (*flags & ~VEH_STATUS_MASK) | VEH_STATUS_ABANDONED;

  *(int *)((uintptr_t)veh + VEH_STATUS) = 1;
  world_add(veh);

  debugPrintf("MENU: spawn %s (model %d, kind %d) at %.1f, %.1f, %.1f\n",
              v->label, v->id, (int)v->kind, vpos[0], vpos[1], vpos[2]);

  snprintf(toast, sizeof(toast), "%s spawned", v->label);
  toast_pending = 1;
}

static void menu_spawn_bodyguards(void) {
  if (!population_add_ped || !choose_gang_occupation || !find_player_ped ||
      !ped_set_player_to_follow) {
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
    // first. Vehicles got this via VehicleCheat's own request/load pair; peds
    // had nothing, which left AddPed constructing against a model that was
    // never streamed in -- the crash had happened before the first log line.
    if (request_model && load_all_models) {
      request_model(model, 1);
      load_all_models(0);
    }

    float pos[3];
    pos[0] = ppos[0] + dx[i];
    pos[1] = ppos[1] + dy[i];
    pos[2] = find_ground_z ? find_ground_z(pos[0], pos[1]) + 1.0f : ppos[2];

    void *guard = population_add_ped(PEDTYPE_GANG1 + BODYGUARD_GANG,
                                     (unsigned)model, pos, 0, 0);
    if (!guard) {
      debugPrintf("MENU: AddPed(model %d) returned NULL\n", model);
      continue;
    }

    ped_set_player_to_follow(guard, 0);
    if (ped_give_weapon)
      ped_give_weapon(guard, BODYGUARD_WEAPON, AMMO_TOPUP, 1);
    if (ped_set_current_weapon)
      ped_set_current_weapon(guard, BODYGUARD_WEAPON);

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
    switch (sub_kind) {
      case SUB_CHEATS:   menu_run_cheat(sub_cursor);    break;
      case SUB_TELEPORT: menu_teleport_to(sub_cursor);  break;
      case SUB_VEHICLES: menu_spawn_vehicle(sub_cursor); break;
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
      menu_dirty = 1;
    }
    return;
  }

  if (pressed & (HidNpadButton_B | HidNpadButton_Minus)) {
    if (sub_kind != SUB_NONE) {
      sub_kind = SUB_NONE;   // back out to the top level rather than closing outright
      menu_dirty = 1;
    } else {
      menu_close();
    }
    return;
  }

  const int count = sub_kind != SUB_NONE ? sub_count() : MENU_TOP_ROWS;
  int *cursor = sub_kind != SUB_NONE ? &sub_cursor : &menu_cursor;
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
  cheat_wanted_level = (cheat_wanted_fn)need_sym("_ZN7CWanted16CheatWantedLevelEi");
  find_ground_z = (find_ground_z_fn)need_sym("_ZN6CWorld19FindGroundZForCoordEff");
  find_player_ped = (find_player_ped_fn)need_sym("_Z13FindPlayerPedv");
  ped_teleport = (ped_teleport_fn)need_sym("_ZN4CPed8TeleportE7CVector");

  ctext_instance = (void **)need_sym("_ZN5CText10msInstanceE");

  gp_the_zones = (void **)need_sym("gpTheZones");
  gp_the_paths = (void **)need_sym("gpThePaths");
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

  vehicle_new = (vehicle_new_fn)need_sym("_ZN8CVehiclenwEm");
  automobile_ctor = (vehicle_ctor_fn)need_sym("_ZN11CAutomobileC1Eih");
  bike_ctor = (vehicle_ctor_fn)need_sym("_ZN5CBikeC1Eih");
  boat_ctor = (vehicle_ctor_fn)need_sym("_ZN5CBoatC1Eih");
  heli_ctor = (vehicle_ctor_fn)need_sym("_ZN5CHeliC1Eih");
  world_add = (world_add_fn)need_sym("_ZN6CWorld3AddEP7CEntity");
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
  ped_set_player_to_follow =
      (set_player_to_follow_fn)need_sym("_ZN4CPed17SetPlayerToFollowEi");
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
