#ifndef MELEE_MN_MNONLINE_H
#define MELEE_MN_MNONLINE_H

#include "forward.h"

#include <sysdolphin/baselib/forward.h>

/* PC only: the VS Mode > Online submenu (MENU_KIND_ONLINE). SdMenu has no
 * label textures or SIS strings for it, so mnmain.c hides the matanim label
 * of every slot that has an mnOnline_Label and draws that text instead. */

/* 22D594-style think proc, see mn_803EB6B0[MENU_KIND_ONLINE]. */
void mnOnline_Think(HSD_GObj*);

/* Literal label / bottom-bar text for PC-only entries; NULL = stock. */
const char* mnOnline_Label(MenuKind, int selection);
const char* mnOnline_Description(MenuKind, int selection);

/* One-shot description override set by the think proc (A on a stub). */
const char* mnOnline_TakeNotice(void);

/* MELEE VS lives on the main menu; its pages are MENU_KIND_MV_* (LOCAL /
 * ONLINE / CREDITS, then LAN / DIRECT / PARTY / TEAM SELECT / MATCHMAKING, the
 * Duo Party page and the credits) and all run mnOnline_Think. */

/* gmmenumode.c, choosing where the menu comes back to. ReturnFromLocal: a
 * scene the MELEE VS page launched with LOCAL returns to that page (true,
 * with the kind and row). ReturnMenu: the page and row that launched an
 * online scene of this OnlineKind. */
bool mnOnline_ReturnFromLocal(int* kind, int* selection);
void mnOnline_ReturnMenu(int online_kind, int* kind, int* selection);

/* Changes whenever a row's text changes under the cursor. A label is built
 * once, so a menu view redraws its labels when this is not the value it saw. */
unsigned mnOnline_LabelGeneration(void);

#endif
