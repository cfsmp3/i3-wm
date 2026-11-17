/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * gestures.c: Touchpad gesture bindings (bindgesture), driven by XInput 2.4
 *             gesture events.
 *
 */
#pragma once

#include <config.h>

/**
 * Adds a gesture binding from config parameters given as strings. Errors in
 * the gesture specification are logged and the binding is ignored.
 *
 * If an identical gesture binding already exists in the given mode, it is
 * replaced by the new binding. Unless no_warn is set, this is logged as an
 * error, just like duplicate key bindings.
 *
 * Returns false if the configuration line contains an error (or replaced an
 * earlier binding without --no-warn), true otherwise.
 *
 */
bool configure_gesture_binding(bool exact, const char *input_device, bool no_warn,
                               const char *gesture, const char *command, const char *modename,
                               bool pango_markup);

/**
 * Removes the gesture binding of the given mode which is the same as the
 * binding described by the config parameters (unbindgesture). Only bindings
 * which were configured before are considered.
 *
 * Returns false if the configuration line contains an error or if there is
 * no such binding, true otherwise.
 *
 */
bool unconfigure_gesture_binding(bool exact, const char *input_device,
                                 const char *gesture, const char *modename);

/**
 * Frees the gesture binding. If bind is NULL, it simply returns.
 *
 */
void gesture_binding_free(GestureBinding *bind);

/**
 * Checks whether the X server supports XInput 2.4 gesture events and, if so,
 * initializes their handling. Needs to be called once at startup, before
 * gestures_grab().
 *
 */
void gestures_init(void);

/**
 * Returns true if the given event is an XInput event handled by
 * handle_gesture_event().
 *
 */
bool gestures_is_gesture_event(int type, xcb_generic_event_t *event);

/**
 * Handles an XInput gesture event: tracks the gesture from its begin event to
 * its end event and runs the best matching gesture binding of the current
 * binding mode once the gesture ends.
 *
 */
void handle_gesture_event(xcb_generic_event_t *event);

/**
 * (Re-)establishes the passive gesture grabs on the root window according to
 * the gesture bindings of the current binding mode: if the mode contains at
 * least one binding for swipes (pinches), all swipes (pinches) are grabbed,
 * otherwise they are passed on to the applications.
 *
 */
void gestures_grab(void);
