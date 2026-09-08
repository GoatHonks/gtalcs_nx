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

// CRadar::ms_RadarTrace: 75 entries of 60 bytes. The layout comes from
// CRadar::SetTargetBlip (0x35b67c), which walks the array testing the byte at
// +43 for a free slot and writes the position as three floats at +12/+16/+20.
#define BLIP_STRIDE 60
#define BLIP_COUNT  75
#define BLIP_INUSE  43
#define BLIP_POS    12
#define BLIP_KIND   56
// Byte 40 is the blip type. A marker you place on the map is a coordinate blip
// (4); the char blip (2) that sits alongside it is a mission contact. Both share
// every other field, which is why the sprite id at +56 was no use as a test.
#define BLIP_TYPE   40
#define BLIP_TYPE_COORD 4

static uint8_t *radar_trace = NULL;
typedef float (*find_ground_z_fn)(float x, float y);
static find_ground_z_fn find_ground_z = NULL;
typedef void *(*find_player_ped_fn)(void);
static find_player_ped_fn find_player_ped = NULL;
// CVector is three floats, so AAPCS64 passes it as a homogeneous float aggregate
// in s0/s1/s2 rather than on the stack -- hence the flattened prototype.
typedef void (*ped_teleport_fn)(void *ped, float x, float y, float z);
static ped_teleport_fn ped_teleport = NULL;

// ---- cheats ----
// Called straight through by symbol. Cheats whose flags nothing in this build
// reads (WallClimbingCheat just eors a byte) are left out rather than listed as
// something that looks like it works.
//
// TankCheat stays: LCS does have a tank (the Rhino), so it does exactly what its
// name says.
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
  { "Spawn tank",       "_Z9TankCheatv",               NULL, 0 },
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
  if (menu_toggle_on[MENU_TOG_NEVER_TIRED] && find_player_ped) {
    void *ped = find_player_ped();
    if (ped) {
      float *energy = (float *)((uintptr_t)ped + PED_SPRINT_ENERGY);
      const float *max = (const float *)((uintptr_t)ped + PED_SPRINT_MAX);
      if (*energy < *max)
        *energy = *max;
    }
  }

  if (!find_player_ped)
    return;

  void *ped = find_player_ped();
  if (!ped)
    return;

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

// ---- menu state ----
#define MENU_ROWS 8

typedef enum {
  MENU_ACT_CHEATS = 0,
  MENU_ACT_TELEPORT,
  MENU_ACT_CLEAR_WANTED,
  MENU_NUM_ACTIONS
} menu_action;

static const char *const menu_action_name[MENU_NUM_ACTIONS] = {
  "Cheats",
  "Teleport to marker",
  "Clear wanted level",
};

// Top level = the actions, a header, then the toggles.
#define MENU_HDR_ROW   MENU_NUM_ACTIONS
#define MENU_TOP_ROWS  (MENU_NUM_ACTIONS + 1 + MENU_NUM_TOGGLES)

static int menu_open = 0;
static int menu_cursor = 0;
static int menu_dirty = 0;
static u64  menu_pad_prev = 0;

// The cheats list, shown as a second level.
static int sub_open = 0;
static int sub_cursor = 0;

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

  if (sub_open) {
    n += snprintf(line + n, sizeof(line) - n, "CHEATS");

    int first = sub_cursor - MENU_ROWS / 2;
    if (first > cheats_ready - MENU_ROWS)
      first = cheats_ready - MENU_ROWS;
    if (first < 0)
      first = 0;

    for (int i = first; i < cheats_ready && i < first + MENU_ROWS; i++) {
      n += snprintf(line + n, sizeof(line) - n, "~n~%c %s",
                    i == sub_cursor ? '>' : ' ', menu_cheats[i].name);
      if (n >= (int)sizeof(line) - 40)
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
  sub_open = 0;
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

static int menu_teleport_ready(void) {
  return radar_trace && find_ground_z && find_player_ped && ped_teleport;
}

// The map marker is somewhere in ms_RadarTrace, but which field identifies it is
// still unconfirmed -- the stamp SetTargetBlip writes (49 at +56) never shows up
// on a real marker, so the live entries are dumped here to work it out.
static int menu_find_marker(float *out_x, float *out_y) {
  int found = 0;
  for (int i = 0; i < BLIP_COUNT; i++) {
    const uint8_t *b = radar_trace + (size_t)i * BLIP_STRIDE;
    if (!b[BLIP_INUSE])
      continue;

    const uint16_t kind = *(const uint16_t *)(b + BLIP_KIND);
    const float x = *(const float *)(b + BLIP_POS);
    const float y = *(const float *)(b + BLIP_POS + 4);

    debugPrintf("MENU: blip %d kind=%u use=%u at %.1f, %.1f | "
                "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                i, (unsigned)kind, (unsigned)b[BLIP_INUSE], x, y,
                b[40], b[41], b[42], b[43], b[44], b[45], b[46], b[47],
                b[48], b[49], b[50], b[51], b[52], b[53], b[54], b[55]);

    if (b[BLIP_TYPE] == BLIP_TYPE_COORD) {
      // Take the last coordinate blip rather than the first: a mission can own
      // one too, and the marker you just placed is the more recent.
      *out_x = x;
      *out_y = y;
      found = 1;
    }
  }
  return found;
}

static void menu_teleport_to_marker(void) {
  if (!menu_teleport_ready()) {
    snprintf(toast, sizeof(toast), "Teleport unavailable");
    toast_pending = 1;
    return;
  }

  float x = 0.0f, y = 0.0f;
  if (!menu_find_marker(&x, &y)) {
    snprintf(toast, sizeof(toast), "Place a marker on the map first");
    toast_pending = 1;
    return;
  }

  void *ped = find_player_ped();
  if (!ped)
    return;

  // Land on the ground rather than inside it, with a little clearance.
  const float z = find_ground_z(x, y) + 1.5f;
  debugPrintf("MENU: teleporting to marker %.1f, %.1f, %.1f\n", x, y, z);
  ped_teleport(ped, x, y, z);

  snprintf(toast, sizeof(toast), "Teleported to marker");
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
  if (sub_open) {
    menu_run_cheat(sub_cursor);
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
      if (cheats_ready) {
        sub_open = 1;
        sub_cursor = 0;
        menu_dirty = 1;
      }
      break;
    case MENU_ACT_TELEPORT:
      menu_teleport_to_marker();
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
      menu_open = 1;
      g_menu_open = 1;
      menu_cursor = 0;
      sub_open = 0;
      menu_dirty = 1;
    }
    return;
  }

  if (pressed & (HidNpadButton_B | HidNpadButton_Minus)) {
    if (sub_open) {
      sub_open = 0;      // back out to the top level rather than closing outright
      menu_dirty = 1;
    } else {
      menu_close();
    }
    return;
  }

  const int count = sub_open ? cheats_ready : MENU_TOP_ROWS;
  int *cursor = sub_open ? &sub_cursor : &menu_cursor;

  // The section header is a label, so the cursor steps over it.
  if (pressed & HidNpadButton_Up) {
    do {
      if (--*cursor < 0)
        *cursor = count - 1;
    } while (!sub_open && *cursor == MENU_HDR_ROW);
    menu_dirty = 1;
  }
  if (pressed & HidNpadButton_Down) {
    do {
      if (++*cursor >= count)
        *cursor = 0;
    } while (!sub_open && *cursor == MENU_HDR_ROW);
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

  radar_trace = (uint8_t *)need_sym("_ZN6CRadar13ms_RadarTraceE");
  ped_set_ammo = (set_ammo_fn)need_sym("_ZN4CPed7SetAmmoE11eWeaponTypej");
  cheat_wanted_level = (cheat_wanted_fn)need_sym("_ZN7CWanted16CheatWantedLevelEi");
  find_ground_z = (find_ground_z_fn)need_sym("_ZN6CWorld19FindGroundZForCoordEff");
  find_player_ped = (find_player_ped_fn)need_sym("_Z13FindPlayerPedv");
  ped_teleport = (ped_teleport_fn)need_sym("_ZN4CPed8TeleportE7CVector");

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

  debugPrintf("MENU: Liberty Menu ready (%d/%d cheats, teleport %s)\n",
              kept, MENU_NUM_CHEATS, menu_teleport_ready() ? "ok" : "unavailable");
}
