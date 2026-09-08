/* config.h -- global configuration and config file handling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// MB reserved for the .so load region (only holds the mapped libGame.so/
// libopenal.so, ~15MB). The newlib heap gets (total RAM - this) and backs all
// dynamic allocation: the game's malloc and mesa's GPU bos. Sizing it larger
// starved the GPU and OOM'd world-load textures (glerr=0x505).
#define MEMORY_SO_MB 256

// The APK ships no libc++_shared.so; the C++ runtime resolves from its own
// libopenal.so, which statically links libc++ with default visibility. We load
// that as a second module purely as the C++ runtime donor. The game's OpenAL
// imports still bind to native openal-soft (import table beats module exports).
#define SO_NAME "libGame.so"
#define CXX_DONOR_SO_NAME "libopenal.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"
// backing store for the engine's get/setAppLocalValue key/value pairs
#define APPSTATE_NAME "appstate.txt"

// Define to write debug.log and nxlink stdout. Off for release (debugPrintf
// becomes a no-op).
#define DEBUG_LOG 1

// actual screen size
extern int screen_width;
extern int screen_height;

// gamepad: input is pushed through the GTAJNIlib JNI entry points.
// onJoyButtonDown/Up take a button index 0..15 (engine bounds-checks #0xf and
// indexes a button-state array); these are the engine's own action ids, not
// tied to any particular physical button. config.txt's key_* entries pick
// which of these each physical Switch button fires.
#define GPAD_BUTTON_A 0
#define GPAD_BUTTON_B 1
#define GPAD_BUTTON_X 2
#define GPAD_BUTTON_Y 3
#define GPAD_BUTTON_START 4
#define GPAD_BUTTON_SELECT 5
#define GPAD_BUTTON_L1 6
#define GPAD_BUTTON_R1 7
#define GPAD_BUTTON_L2 8
#define GPAD_BUTTON_R2 9
#define GPAD_BUTTON_DPAD_LEFT 10
#define GPAD_BUTTON_DPAD_RIGHT 11
#define GPAD_BUTTON_DPAD_UP 12
#define GPAD_BUTTON_DPAD_DOWN 13
#define GPAD_BUTTON_THUMBL 14
#define GPAD_BUTTON_THUMBR 15

// The names above came from the Android keycode list, and four of them are wrong
// about what the engine does with them. CPad::Update() maps each id onto a fixed
// CControllerState field, and in THAT table id 8/9 are the real d-pad up/down --
// the fields CPad::SniperZoomIn/Out, CycleCameraMode{Up,Down}JustDown, GetHorn,
// GuiUp/GuiDown and CMenuManager all read -- while ids 12/13 land on fields almost
// nothing reads (13 only feeds CCam::Process_FollowCar_SA's recentre-behind-car).
// Real L2/R2 are not in this table at all: they are the analogue triggers, which
// reach the engine through the LT/RT axes instead. Use these aliases in new code;
// the spellings above stay valid so existing config.txt files keep parsing.
#define GPAD_ACTION_DPAD_UP     GPAD_BUTTON_L2           // id 8
#define GPAD_ACTION_DPAD_DOWN   GPAD_BUTTON_R2           // id 9
#define GPAD_ACTION_DPAD_LEFT   GPAD_BUTTON_DPAD_LEFT    // id 10
#define GPAD_ACTION_DPAD_RIGHT  GPAD_BUTTON_DPAD_RIGHT   // id 11
#define GPAD_ACTION_UNUSED      GPAD_BUTTON_DPAD_UP      // id 12 -- no reader
#define GPAD_ACTION_CAM_CENTER  GPAD_BUTTON_DPAD_DOWN    // id 13 -- recentre behind car

// sentinel: physical button fires nothing (lets a button be disabled)
#define GPAD_BUTTON_NONE (-1)
// sentinel: physical button sounds the horn. The horn field in CControllerState
// is shared with the logical d-pad (CPad::GetHorn reads the same 0x12/0x14 the
// camera cycling does), so there is no id that means "horn and nothing else" --
// this is handled by a hook on CPad::GetHorn instead of a joybutton event.
#define GPAD_BUTTON_HORN (-4)
// sentinel: physical button triggers the engine's back/pause action instead
// of a normal joybutton event (this is Minus's default)
#define GPAD_BUTTON_BACK (-3)

typedef struct {
  int screen_width;
  int screen_height;
  int trilinear_filter;
  int show_fps;
  int psp_layout;    // face buttons. 1 (default) = positional, matching the PSP: the
                      // top button is Triangle, left is Square, right is Circle, bottom is
                      // Cross -- so a printed PSP cheat code can be entered by shape.
                      // 0 = Nintendo confirm/cancel convention (A = Cross, B = Circle).
                      // Only picks DEFAULTS for key_a/b/x/y; explicit lines still win.
                      // Accepted under its old name "xbox_layout" too.

  // per-physical-button remap, one engine action (GPAD_BUTTON_* name, or NONE) per
  // Switch button. Defaults: L/R/ZL/ZR/dpad/sticks/plus match the original fixed
  // mapping; A/B/X/Y default from psp_layout above unless set explicitly here.
  int key_a, key_b, key_x, key_y;
  int key_l, key_r, key_zl, key_zr;
  int key_up, key_down, key_left, key_right;
  int key_lstick, key_rstick, key_plus, key_minus;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
