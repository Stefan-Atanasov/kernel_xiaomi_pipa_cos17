/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PIPA_PEN_BUTTONS_H
#define _PIPA_PEN_BUTTONS_H

/* the display sent a pen frame: the pen is in hover range now */
void pipa_pen_frame(void);
/* buttons whose press started with the pen away (bit 0 front, bit 1 back): the display does not report them */
unsigned int pipa_pen_far_mask(void);

#endif
