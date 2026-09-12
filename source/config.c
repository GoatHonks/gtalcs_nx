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
  c->key_minus = GPAD_BUTTON_NONE; // Liberty Menu's button; see read_config
}

static void config_resolve_faces(Config *c) {
  if (c->key_a == KEY_UNSET) c->key_a = c->psp_layout ? GPAD_BUTTON_B : GPAD_BUTTON_A;
  if (c->key_b == KEY_UNSET) c->key_b = c->psp_layout ? GPAD_BUTTON_A : GPAD_BUTTON_B;
  if (c->key_x == KEY_UNSET) c->key_x = c->psp_layout ? GPAD_BUTTON_Y : GPAD_BUTTON_X;
  if (c->key_y == KEY_UNSET) c->key_y = c->psp_layout ? GPAD_BUTTON_X : GPAD_BUTTON_Y;
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

  // Every setting is written out with a comment describing it, so config.txt
  // documents itself and there is nothing to look up elsewhere. read_config()
  // skips any line starting with '#'.
  //
  // The file is rewritten on every launch: your values are kept, but any
  // comments you add yourself are not.
  #define CONFIG_BUTTON_LINE(var) do { \
    const char *nm = button_token_name(config.var); \
    if (nm) fprintf(f, "%s %s\n", #var, nm); \
    else fprintf(f, "%s %d\n", #var, config.var); \
  } while (0)

  // key_a/b/x/y have no fixed default: they follow psp_layout unless config.txt
  // names them. Writing them out unconditionally would pin them to whatever the
  // layout resolved to this boot, and psp_layout would then appear to do nothing
  // on the next one. So while a face button still matches its layout default the
  // line is written commented out -- visible and ready to edit, but not binding.
  // A remap actually chosen differs from the default, so it is written for real.
  #define CONFIG_FACE_LINE(var, dflt) do { \
    const char *nm = button_token_name(config.var); \
    const char *pfx = (config.var == (dflt)) ? "#" : ""; \
    if (nm) fprintf(f, "%s%s %s\n", pfx, #var, nm); \
    else fprintf(f, "%s%s %d\n", pfx, #var, config.var); \
  } while (0)

  fprintf(f, "# gtalcs_nx configuration\n");

  fprintf(f, "\n# Render size. -1 = auto: 720p handheld, 1080p docked.\n");
  fprintf(f, "screen_width %d\n", config.screen_width);
  fprintf(f, "screen_height %d\n", config.screen_height);

  fprintf(f, "\n# Trilinear texture filtering. 0 is slightly sharper and slightly\n"
             "# faster, 1 is smoother.\n");
  fprintf(f, "trilinear_filter %d\n", config.trilinear_filter);

  fprintf(f, "\n# Small FPS counter in the top-left corner. 1 = on.\n");
  fprintf(f, "show_fps %d\n", config.show_fps);

  fprintf(f, "\n# Face button layout.\n"
             "#   1 = positional, matching the PSP: X = Triangle (top), Y = Square\n"
             "#       (left), A = Circle (right), B = Cross (bottom). A printed PSP\n"
             "#       cheat code can then be entered by shape.\n"
             "#   0 = Nintendo confirm/cancel convention: A = Cross, B = Circle.\n");
  fprintf(f, "psp_layout %d\n", config.psp_layout);

  fprintf(f, "\n# Button mapping. One entry per physical Switch button; the value is an\n"
             "# engine ACTION, not another button, so several buttons may share one.\n"
             "# Valid actions:\n"
             "#   A           Cross    - sprint on foot, accelerate in a vehicle\n"
             "#   B           Circle   - attack / fire weapon, car weapon\n"
             "#   X           Square   - jump, brake / reverse\n"
             "#   Y           Triangle - enter vehicle, skip phone call\n"
             "#   L1                   - answer phone, collect pickup, sub-mission\n"
             "#   R1                   - target / scope view, hand brake\n"
             "#   DPAD_UP DPAD_DOWN    - cycle camera, scope zoom, horn\n"
             "#   DPAD_LEFT DPAD_RIGHT - cycle weapon / target, radio stations\n"
             "#   START                - pause menu\n"
             "#   BACK                 - back / pause (Minus's default)\n"
             "#   HORN                 - horn only, without the d-pad side effects\n"
             "#   CAM_CENTER           - recentre the camera behind the car\n"
             "#   NONE                 - button does nothing\n");

  fprintf(f, "\n# The four face buttons follow psp_layout above. These lines show what it\n"
             "# resolved to; uncomment one to pin that button regardless of the layout.\n");
  CONFIG_FACE_LINE(key_a, config.psp_layout ? GPAD_BUTTON_B : GPAD_BUTTON_A);
  CONFIG_FACE_LINE(key_b, config.psp_layout ? GPAD_BUTTON_A : GPAD_BUTTON_B);
  CONFIG_FACE_LINE(key_x, config.psp_layout ? GPAD_BUTTON_Y : GPAD_BUTTON_X);
  CONFIG_FACE_LINE(key_y, config.psp_layout ? GPAD_BUTTON_X : GPAD_BUTTON_Y);

  fprintf(f, "\n# Shoulders, d-pad, stick clicks and Plus/Minus.\n");
  CONFIG_BUTTON_LINE(key_l);
  CONFIG_BUTTON_LINE(key_r);
  CONFIG_BUTTON_LINE(key_zl);
  CONFIG_BUTTON_LINE(key_zr);
  CONFIG_BUTTON_LINE(key_up);
  CONFIG_BUTTON_LINE(key_down);
  CONFIG_BUTTON_LINE(key_left);
  CONFIG_BUTTON_LINE(key_right);
  CONFIG_BUTTON_LINE(key_lstick);
  CONFIG_BUTTON_LINE(key_rstick);
  CONFIG_BUTTON_LINE(key_plus);
  fprintf(f,
          "\n# Minus opens Liberty Menu. You can bind a game action to it as\n"
          "# well, but NONE is recommended and is the default: anything else\n"
          "# fires that action at the same moment the menu opens. The port's\n"
          "# own default here was BACK, which duplicates Plus, so leaving it\n"
          "# at NONE costs nothing.\n");
  CONFIG_BUTTON_LINE(key_minus);

  #undef CONFIG_FACE_LINE
  #undef CONFIG_BUTTON_LINE

  fclose(f);

  return 0;
}
