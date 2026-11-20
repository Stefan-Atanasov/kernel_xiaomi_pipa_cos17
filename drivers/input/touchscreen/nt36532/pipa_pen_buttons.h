/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PIPA_PEN_BUTTONS_H
#define _PIPA_PEN_BUTTONS_H

/* the display sent a pen frame: the pen is in hover range now */
void pipa_pen_frame(void);
/* raw display buttons (bit 0 front, bit 1 back) -> the ones the display may report (see pipa_pen_buttons.c) */
unsigned int pipa_pen_display_buttons(unsigned int raw);

/* BUGS 155: the keyboard's Super key pressed alone (nanosic driver) -> KEY_F18 on the virtual device for the ROM */
void pipa_vkey_super(void);

#endif
