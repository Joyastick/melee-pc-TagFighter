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

/* MELEE VS's own submenu reuses MENU_KIND_ONLINE's rendering wholesale
 * (same banner, same preview animations) with a different row set (Local /
 * Direct / Unranked instead of LAN / Direct / Ranked / Unranked / Profile).
 * true selects the Tag Battle row set and Back target; mn_8022D594 sets it
 * before entering MENU_KIND_ONLINE from either VS row, so it never carries
 * over from an earlier visit through the other row. */
void mnOnline_SetEnteredFromTagBattle(bool value);

#endif
