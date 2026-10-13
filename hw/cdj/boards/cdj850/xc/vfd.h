/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 VFD text display (deck window, text, not pixels). */
#ifndef CDJ850_VFD_H
#define CDJ850_VFD_H
#include "cdj_common.h"

/* Create the deck window. Safe headless (no window without a display). */
void vfd_init(void);

/* Accessor: the two display strings (DATA FL line, JOG FL ring state).
 * Always NUL-terminated; callers provide the buffers. */
void vfd_text(char *line0, unsigned n0, char *line1, unsigned n1);

/* Fed by panel_link.c for every MAIN->panel byte (FL frame capture). */
void vfd_note_tx(uint8_t b);

#endif
