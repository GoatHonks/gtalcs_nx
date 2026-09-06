/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "config.h"

// sentinel used only while resolving defaults: "not set explicitly, derive
// from xbox_layout" (kept out of config.h since callers never see it)
#define KEY_UNSET (-2)

#define CONFIG_VARS \
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_INT(trilinear_filter); \
  CONFIG_VAR_INT(show_fps); \
  CONFIG_VAR_OPT_INT(psp_layout); \
  CONFIG_VAR_BUTTON(key_a); \
  CONFIG_VAR_BUTTON(key_b); \
  CONFIG_VAR_BUTTON(key_x); \
  CONFIG_VAR_BUTTON(key_y); \
  CONFIG_VAR_BUTTON(key_l); \
  CONFIG_VAR_BUTTON(key_r); \
  CONFIG_VAR_BUTTON(key_zl); \
  CONFIG_VAR_BUTTON(key_zr); \
  CONFIG_VAR_BUTTON(key_up); \
  CONFIG_VAR_BUTTON(key_down); \
  CONFIG_VAR_BUTTON(key_left); \
  CONFIG_VAR_BUTTON(key_right); \
  CONFIG_VAR_BUTTON(key_lstick); \
  CONFIG_VAR_BUTTON(key_rstick); \
  CONFIG_VAR_BUTTON(key_plus); \
  CONFIG_VAR_BUTTON(key_minus);

Config config;


// actual screen size in use
int screen_width = 1280;
int screen_height = 720;

// text used in config.txt for each engine action, so key_* lines read like
// "key_zl B" instead of a bare number nobody can remember
static const struct { const char *name; int val; } button_names[] = {
  { "A", GPAD_BUTTON_A }, { "B", GPAD_BUTTON_B },
  { "X", GPAD_BUTTON_X }, { "Y", GPAD_BUTTON_Y },
  { "START", GPAD_BUTTON_START }, { "SELECT", GPAD_BUTTON_SELECT },
  { "L1", GPAD_BUTTON_L1 }, { "R1", GPAD_BUTTON_R1 },
  // ids 8/9 really are the engine's d-pad up/down -- see the note in config.h
  { "DPAD_UP", GPAD_ACTION_DPAD_UP }, { "DPAD_DOWN", GPAD_ACTION_DPAD_DOWN },
  { "DPAD_LEFT", GPAD_ACTION_DPAD_LEFT }, { "DPAD_RIGHT", GPAD_ACTION_DPAD_RIGHT },
  { "UNUSED", GPAD_ACTION_UNUSED }, { "CAM_CENTER", GPAD_ACTION_CAM_CENTER },
  { "THUMBL", GPAD_BUTTON_THUMBL }, { "THUMBR", GPAD_BUTTON_THUMBR },
  { "NONE", GPAD_BUTTON_NONE }, { "BACK", GPAD_BUTTON_BACK },
  { "HORN", GPAD_BUTTON_HORN },
  // Deprecated spellings kept so an existing config.txt still parses. They are
  // last so button_token_name() writes the corrected names back out instead.
  { "L2", GPAD_BUTTON_L2 }, { "R2", GPAD_BUTTON_R2 },
};
#define NUM_BUTTON_NAMES (sizeof(button_names) / sizeof(*button_names))

// accepts either a known name (case-insensitive) or a raw number, so old
// hand-edited numeric configs keep working
static int parse_button_token(const char *value) {
  for (unsigned i = 0; i < NUM_BUTTON_NAMES; i++)
    if (!strcasecmp(value, button_names[i].name))
      return button_names[i].val;
  return atoi(value);
}

static const char *button_token_name(int val) {
  for (unsigned i = 0; i < NUM_BUTTON_NAMES; i++)
    if (button_names[i].val == val)
      return button_names[i].name;
  return NULL; // unrecognised value; caller falls back to printing the number
}

static inline void parse_var(const char *name, const char *value) {
  // legacy spelling -- "xbox_layout 1" always meant this same positional mapping
  // legacy spelling. Deliberately NOT marked as seen: it is dropped on the next
  // rewrite so an old config.txt stops overriding the PSP default silently.
  if (!strcmp(name, "xbox_layout")) { config.psp_layout = atoi(value); return; }
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_OPT_INT(var) CONFIG_VAR_INT(var)
  #define CONFIG_VAR_FLOAT(var) if (!strcmp(name, #var)) { config.var = atof(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { strlcpy(config.var, value, sizeof(config.var)); return; }
  #define CONFIG_VAR_BUTTON(var) if (!strcmp(name, #var)) { config.var = parse_button_token(value); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_OPT_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR
  #undef CONFIG_VAR_BUTTON
}

static void config_set_defaults(Config *c) {
  c->screen_width = -1; // auto
  c->screen_height = -1;
  c->trilinear_filter = 1;
  c->show_fps = 0; // small FPS counter in the top left corner
  // 1 = positional/PSP faces (top=Triangle, left=Square, right=Circle, bottom=Cross)
  c->psp_layout = 1;

  // A/B/X/Y start unresolved so we can tell "not set in config.txt" apart
  // from "set to A/B/X/Y explicitly", and fall back to psp_layout below.
  // Everything else has one fixed default, same as before this option existed.
  c->key_a = c->key_b = c->key_x = c->key_y = KEY_UNSET;
  c->key_l = GPAD_BUTTON_L1;
  c->key_r = GPAD_BUTTON_R1;
  // These are engine action ids, not physical buttons -- see the id-table note in
  // config.h for why the d-pad ids are the ones they are.
  c->key_up = GPAD_ACTION_DPAD_UP;
  c->key_down = GPAD_ACTION_DPAD_DOWN;
  c->key_left = GPAD_ACTION_DPAD_LEFT;
  c->key_right = GPAD_ACTION_DPAD_RIGHT;
  // ZL/ZR are the analogue triggers: on foot ZR alone is look-behind, in a car
  // ZL = look left, ZR = look right and both together = look behind. All of that
  // is driven by CPad fields 0x0c/0x10, which main.c feeds from the LT/RT axes
  // (threshold 0.8) independently of this id table -- so no button id here.
  c->key_zl = GPAD_BUTTON_NONE;
  c->key_zr = GPAD_BUTTON_NONE;
  // The PSP original has no L3/R3, so THUMBL/THUMBR reach nothing in CPad and
  // both sticks would otherwise be dead. The PS2 release puts the horn on L3, so
  // do the same via the HORN sentinel; R3 gets the one id that still has a reader
  // (DPAD_DOWN's field is what CCam::Process_FollowCar_SA polls to recentre the
  // camera behind a car).
  c->key_lstick = GPAD_BUTTON_HORN;
  c->key_rstick = GPAD_ACTION_CAM_CENTER;
  c->key_plus = GPAD_BUTTON_START;
  c->key_minus = GPAD_BUTTON_BACK; // preserves the old hardcoded behaviour
}

static void config_resolve_faces(Config *c) {
  if (c->key_a == KEY_UNSET) c->key_a = c->psp_layout ? GPAD_BUTTON_B : GPAD_BUTTON_A;
  if (c->key_b == KEY_UNSET) c->key_b = c->psp_layout ? GPAD_BUTTON_A : GPAD_BUTTON_B;
  if (c->key_x == KEY_UNSET) c->key_x = c->psp_layout ? GPAD_BUTTON_Y : GPAD_BUTTON_X;
  if (c->key_y == KEY_UNSET) c->key_y = c->psp_layout ? GPAD_BUTTON_X : GPAD_BUTTON_Y;
}

// The defaults, resolved the same way read_config() resolves them, so
// write_config() can leave out every line that just restates one.
static void config_defaults_for_compare(Config *d, int psp_layout) {
  memset(d, 0, sizeof(*d));
  config_set_defaults(d);
  d->psp_layout = psp_layout;   // compare against the layout actually in use
  config_resolve_faces(d);
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  config_set_defaults(&config);

  FILE *f = fopen(file, "r");
  if (f != NULL) {
    // each line is either a '#' comment or "NAME VALUE" (surrounding space ok)
    do {
      char *name = NULL, *value = NULL, *tmp = NULL;
      if (fgets(line, sizeof(line), f) != NULL) {
        name = line;
        while (*name && isspace((int)*name)) ++name;
        if (name[0] == '#') continue;
        for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
        // no whitespace after the name means no value
        if (*tmp != 0) {
          *tmp = 0;
          for (value = tmp + 1; *value && isspace((int)*value); ++value);
          for (tmp = value + strlen(value) - 1; isspace((int)*tmp); --tmp) *tmp = 0;
          parse_var(name, value);
        }
      }
    } while (!feof(f));

    fclose(f);
  }

  // resolve any face button not set explicitly in config.txt, from psp_layout
  // ids 0/1/2/3 are the engine's Cross/Circle/Square/Triangle -- confirmed by the
  // characters CPad::DoCheats() feeds to AddToCheatString ('X','C','S','T').
  // psp_layout maps them by position so the shapes land where a PSP player expects.
  config_resolve_faces(&config);

  return f != NULL ? 0 : -1;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  // Anything that just restates a default is left out, so a stock config.txt has
  // no psp_layout line and none of the sixteen key_* lines. A remap somebody
  // actually chose differs from the default, so it still survives the rewrite.
  Config d;
  config_defaults_for_compare(&d, config.psp_layout);
  Config dl;
  config_defaults_for_compare(&dl, 1);
  d.psp_layout = dl.psp_layout;   // psp_layout itself is written only if != 1

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_OPT_INT(var) if (config.var != d.var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_FLOAT(var) fprintf(f, "%s %g\n", #var, config.var)
  #define CONFIG_VAR_STR(var) if (config.var[0]) fprintf(f, "%s %s\n", #var, config.var)
  #define CONFIG_VAR_BUTTON(var) do { \
    if (config.var != d.var) { \
      const char *nm = button_token_name(config.var); \
      if (nm) fprintf(f, "%s %s\n", #var, nm); else fprintf(f, "%s %d\n", #var, config.var); \
    } \
  } while (0)
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_OPT_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR
  #undef CONFIG_VAR_BUTTON

  fclose(f);

  return 0;
}
