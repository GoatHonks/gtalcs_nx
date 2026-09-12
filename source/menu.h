/* menu.h -- Liberty Menu
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __MENU_H__
#define __MENU_H__

// Resolves the game entry points the menu needs. Called from patch_game().
void menu_init(void);

// One tick per frame, from the main loop. `in_game` is the port's app state 9;
// the menu stays shut during boot and the front end, where the text system it
// draws through is not up yet.
void menu_tick(int in_game);

#endif
