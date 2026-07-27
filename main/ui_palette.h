#pragma once

/* Colours shared across screens.
 *
 * Only what more than one file needs. A screen's own hues stay in that screen's
 * file — collecting every colour here would make a retheme read as a global
 * edit and hide which screen each value belongs to.
 *
 * This header existed once before and was deleted, correctly, when the storm
 * screen's predecessor took the last of its second consumers away. It is back
 * because the storm screen redefined the same two values verbatim — including
 * one comment word for word — which is exactly the drift the first version was
 * written to stop, recurring within a day of its deletion. The rule was right;
 * the file simply had no work to do for a while. */
#define UI_COLOR_ACCENT 0x00E676  /* the device green */
#define UI_COLOR_TRACK  0x1A2E22  /* arc groove, a dark cast of the accent */
