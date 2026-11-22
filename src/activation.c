/*
 * activation.c - xdg-activation-v1: what a window that asks for the focus with a token
 * gets. main.c tells what it knows of the token and the window and acts. Tested in
 * tests/unit.
 */
#include "mwc.h"

/*
 * rediwm's rule: only a token made while its window had the keyboard, for an input
 * event the client got, takes the focus; a program in the background can't. Panels and
 * launchers count as that window. Nor once the keys went elsewhere: a slow browser
 * must not take them from the editor typed in since the link was clicked, nor from a
 * screen locker that came up meanwhile. Such a token brings the window back from an
 * icon and switches to its desktop. Any other token marks the desktop of a window on
 * another one, as a dialog opening there does; on the current desktop it does nothing.
 * mwt's bell asks without an input event. Docked windows get the focus by a click only.
 */
enum activation activation_decide(const struct activation_request *r)
{
	if (!r->target_mapped || r->target_docked)
		return ACTIVATION_NONE;
	bool good = r->from_input && r->source_had_focus && r->focus_kept;
	if (!good)
		return r->target_off_desktop ? ACTIVATION_MARK : ACTIVATION_NONE;
	return r->target_off_desktop ? ACTIVATION_SWITCH : ACTIVATION_FOCUS;
}
