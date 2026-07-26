#pragma once

/* Colours shared across screens.
 *
 * Only what more than one file needs. A screen's own hues stay in that screen's
 * file — collecting every colour here would make a retheme read as a global
 * edit and hide which screen each value belongs to.
 *
 * UI_COLOR_ACCENT is the device green. It was named in the first place so a
 * retheme is one edit rather than a hunt; it then acquired a second definition
 * in ui_status.c, whose comment said "matches ui_test.c's" — which is the
 * property this header exists to keep true rather than to document. */
#define UI_COLOR_ACCENT 0x00E676
