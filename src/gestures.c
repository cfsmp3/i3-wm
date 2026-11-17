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
#include "all.h"

#include <math.h>

#include <xcb/xinput.h>

/* The XInput major opcode, or -1 if the X server does not support XInput 2.4
 * gesture events (in which case gesture bindings never trigger). */
static int xinput_opcode = -1;

/* Which kinds of gestures are currently grabbed on the root window. */
static bool pinch_grabbed = false;
static bool swipe_grabbed = false;

/* Thresholds used to derive the directions of a pinch gesture. */
#define GESTURE_PINCH_INWARD_SCALE 0.9
#define GESTURE_PINCH_OUTWARD_SCALE 1.1
#define GESTURE_PINCH_ROTATION_DEGREES 5.0

/*
 * A gesture which is in progress, from its begin event to its end event.
 *
 */
struct gesture_tracker {
    gesture_type_t type;
    /* The (slave) device the gesture originates from. */
    xcb_input_device_id_t sourceid;
    /* The XInput name of the source device, NULL if unknown. */
    char *device_name;
    /* The number of fingers, as reported by the X server in the begin
     * event. */
    uint32_t fingers;
    /* Summed up displacement of all updates. */
    double dx;
    double dy;
    /* The most recently reported scale (pinch only). */
    double scale;
    /* Summed up rotation of all updates, in degrees (pinch only). */
    double rotation;

    TAILQ_ENTRY(gesture_tracker) trackers;
};

static TAILQ_HEAD(gesture_trackers_head, gesture_tracker) gesture_trackers =
    TAILQ_HEAD_INITIALIZER(gesture_trackers);

static const struct {
    const char *name;
    gesture_direction_t direction;
} gesture_directions[] = {
    {"up", GESTURE_DIRECTION_UP},
    {"down", GESTURE_DIRECTION_DOWN},
    {"left", GESTURE_DIRECTION_LEFT},
    {"right", GESTURE_DIRECTION_RIGHT},
    {"inward", GESTURE_DIRECTION_INWARD},
    {"outward", GESTURE_DIRECTION_OUTWARD},
    {"clockwise", GESTURE_DIRECTION_CLOCKWISE},
    {"counterclockwise", GESTURE_DIRECTION_COUNTERCLOCKWISE},
};

static bool parse_gesture_type(const char *str, gesture_type_t *type) {
    if (strcmp(str, "pinch") == 0) {
        *type = GESTURE_TYPE_PINCH;
    } else if (strcmp(str, "hold") == 0) {
        *type = GESTURE_TYPE_HOLD;
    } else if (strcmp(str, "swipe") == 0) {
        *type = GESTURE_TYPE_SWIPE;
    } else {
        return false;
    }
    return true;
}

/*
 * A finger count is a single digit from 1 to 9.
 *
 */
static bool parse_gesture_fingers(const char *str, uint8_t *fingers) {
    if (str[0] < '1' || str[0] > '9' || str[1] != '\0') {
        return false;
    }
    *fingers = str[0] - '0';
    return true;
}

/*
 * Parses a list of directions separated by +, e.g. "up+inward". The keyword
 * "any" does not add any direction.
 *
 */
static bool parse_gesture_directions(const char *str, uint32_t *directions) {
    bool result = true;
    char *copy = sstrdup(str);
    char *walk = copy;
    *directions = GESTURE_DIRECTION_NONE;
    while (walk != NULL) {
        char *next = strchr(walk, '+');
        if (next != NULL) {
            *(next++) = '\0';
        }

        bool found = (strcmp(walk, "any") == 0);
        for (size_t i = 0; !found && i < sizeof(gesture_directions) / sizeof(gesture_directions[0]); i++) {
            if (strcmp(walk, gesture_directions[i].name) == 0) {
                *directions |= gesture_directions[i].direction;
                found = true;
            }
        }
        if (!found) {
            ELOG("Unknown gesture direction \"%s\"\n", walk);
            result = false;
            break;
        }
        walk = next;
    }
    free(copy);
    return result;
}

/*
 * Parses a gesture specification of the form <type>[:<fingers>][:<directions>]
 * into the given binding.
 *
 */
static bool parse_gesture(const char *gesture, GestureBinding *bind) {
    char *segments[3] = {NULL, NULL, NULL};
    int num_segments = 0;
    bool result = false;

    char *copy = sstrdup(gesture);
    char *walk = copy;
    while (walk != NULL) {
        if (num_segments == 3) {
            ELOG("Gesture \"%s\" has more than three segments separated by ':'\n", gesture);
            goto out;
        }
        segments[num_segments++] = walk;
        if ((walk = strchr(walk, ':')) != NULL) {
            *(walk++) = '\0';
        }
    }

    if (!parse_gesture_type(segments[0], &(bind->type))) {
        ELOG("Unknown gesture type \"%s\" (expected pinch, hold or swipe)\n", segments[0]);
        goto out;
    }

    if (num_segments == 3) {
        if (!parse_gesture_fingers(segments[1], &(bind->fingers))) {
            ELOG("Invalid number of fingers \"%s\" in gesture \"%s\" (expected 1 to 9)\n",
                 segments[1], gesture);
            goto out;
        }
        if (!parse_gesture_directions(segments[2], &(bind->directions))) {
            goto out;
        }
    } else if (num_segments == 2) {
        /* A single digit is the number of fingers, anything else is a list
         * of directions. */
        if (!parse_gesture_fingers(segments[1], &(bind->fingers)) &&
            !parse_gesture_directions(segments[1], &(bind->directions))) {
            goto out;
        }
    }

    result = true;

out:
    free(copy);
    return result;
}

/*
 * Returns true if both gesture bindings would be triggered by the very same
 * gestures with the same priority, i.e. if one is a duplicate of the other.
 *
 */
static bool gesture_binding_same(const GestureBinding *a, const GestureBinding *b) {
    if (a->type != b->type ||
        a->fingers != b->fingers ||
        a->directions != b->directions ||
        a->exact != b->exact ||
        a->continuous != b->continuous) {
        return false;
    }
    if (a->input_device == NULL || b->input_device == NULL) {
        return (a->input_device == b->input_device);
    }
    return (strcmp(a->input_device, b->input_device) == 0);
}

/*
 * Creates a gesture binding from config parameters, without a command. Returns
 * NULL if the gesture specification contains an error.
 *
 */
static GestureBinding *gesture_binding_from_config(bool exact, bool continuous, const char *input_device,
                                                   const char *gesture, const char *directive) {
    if (gesture[0] == '-') {
        ELOG("Unknown option \"%s\" for %s, ignoring this line.\n", gesture, directive);
        return NULL;
    }

    GestureBinding *bind = scalloc(1, sizeof(GestureBinding));
    if (!parse_gesture(gesture, bind)) {
        ELOG("Could not parse gesture \"%s\", ignoring this line.\n", gesture);
        FREE(bind);
        return NULL;
    }

    bind->exact = exact;
    bind->continuous = continuous;
    /* "*" means all devices, which is the same as not specifying a device. */
    if (input_device != NULL && strcmp(input_device, "*") != 0) {
        bind->input_device = sstrdup(input_device);
    }
    bind->symbol = sstrdup(gesture);

    return bind;
}

/*
 * Returns the gesture binding of the given mode which is the same as the given
 * binding (see gesture_binding_same()), or NULL if there is none.
 *
 */
static GestureBinding *find_same_gesture_binding(const struct Mode *mode, const GestureBinding *bind) {
    GestureBinding *current;
    TAILQ_FOREACH (current, mode->gesture_bindings, gesture_bindings) {
        if (gesture_binding_same(current, bind)) {
            return current;
        }
    }
    return NULL;
}

/*
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
bool configure_gesture_binding(bool exact, bool continuous, const char *input_device, bool no_warn,
                               const char *gesture, const char *command, const char *modename,
                               bool pango_markup) {
    DLOG("Gesture binding %s, exact %d, continuous %d, input device %s, no-warn %d, command %s\n",
         gesture, exact, continuous, input_device ? input_device : "*", no_warn, command);

    GestureBinding *new_binding = gesture_binding_from_config(exact, continuous, input_device, gesture, "bindgesture");
    if (new_binding == NULL) {
        return false;
    }
    new_binding->command = sstrdup(command);

    bool result = true;
    const struct Mode *mode = mode_from_name(modename, pango_markup);
    GestureBinding *current = find_same_gesture_binding(mode, new_binding);
    if (current != NULL) {
        if (!no_warn) {
            ELOG("Duplicate gesture binding in config file (mode \"%s\"):\n"
                 "  gesture %s, command \"%s\" overwrites gesture %s, command \"%s\"\n",
                 modename, new_binding->symbol, new_binding->command,
                 current->symbol, current->command);
            result = false;
        } else {
            DLOG("Gesture binding %s overwrites %s\n", new_binding->symbol, current->symbol);
        }

        /* The new binding takes the place of the line which replaced the
         * earlier binding, i.e. it is appended below. */
        TAILQ_REMOVE(mode->gesture_bindings, current, gesture_bindings);
        gesture_binding_free(current);
    }

    TAILQ_INSERT_TAIL(mode->gesture_bindings, new_binding, gesture_bindings);

    return result;
}

/*
 * Removes the gesture binding of the given mode which is the same as the
 * binding described by the config parameters (unbindgesture). Only bindings
 * which were configured before are considered.
 *
 * Returns false if the configuration line contains an error or if there is
 * no such binding, true otherwise.
 *
 */
bool unconfigure_gesture_binding(bool exact, bool continuous, const char *input_device,
                                 const char *gesture, const char *modename) {
    DLOG("Removing gesture binding %s, exact %d, continuous %d, input device %s\n",
         gesture, exact, continuous, input_device ? input_device : "*");

    GestureBinding *removed = gesture_binding_from_config(exact, continuous, input_device, gesture, "unbindgesture");
    if (removed == NULL) {
        return false;
    }

    GestureBinding *current = NULL;
    struct Mode *mode;
    SLIST_FOREACH (mode, &modes, modes) {
        if (strcmp(mode->name, modename) == 0) {
            current = find_same_gesture_binding(mode, removed);
            break;
        }
    }

    bool result = true;
    if (current == NULL) {
        ELOG("Cannot remove gesture binding %s (mode \"%s\"): there is no such binding.\n",
             removed->symbol, modename);
        result = false;
    } else {
        DLOG("Removed gesture binding %s, command \"%s\"\n", current->symbol, current->command);
        TAILQ_REMOVE(mode->gesture_bindings, current, gesture_bindings);
        gesture_binding_free(current);
    }

    gesture_binding_free(removed);
    return result;
}

/*
 * Frees the gesture binding. If bind is NULL, it simply returns.
 *
 */
void gesture_binding_free(GestureBinding *bind) {
    if (bind == NULL) {
        return;
    }

    FREE(bind->input_device);
    FREE(bind->symbol);
    FREE(bind->command);
    FREE(bind);
}

/*
 * Checks whether the X server supports XInput 2.4 gesture events and, if so,
 * initializes their handling. Needs to be called once at startup, before
 * gestures_grab().
 *
 */
void gestures_init(void) {
    const xcb_query_extension_reply_t *extreply = xcb_get_extension_data(conn, &xcb_input_id);
    if (extreply == NULL || !extreply->present) {
        DLOG("XInput is not present on this server, gesture bindings are disabled.\n");
        return;
    }

    xcb_input_xi_query_version_reply_t *version = xcb_input_xi_query_version_reply(
        conn, xcb_input_xi_query_version(conn, 2, 4), NULL);
    if (version == NULL) {
        ELOG("Could not query the XInput version, gesture bindings are disabled.\n");
        return;
    }

    const bool supported = (version->major_version > 2 ||
                            (version->major_version == 2 && version->minor_version >= 4));
    DLOG("XInput version %d.%d\n", version->major_version, version->minor_version);
    free(version);

    if (!supported) {
        LOG("XInput 2.4 is not supported by the X server, gesture bindings are disabled.\n");
        return;
    }

    xinput_opcode = extreply->major_opcode;
}

/*
 * Returns true if the given event is an XInput event handled by
 * handle_gesture_event().
 *
 */
bool gestures_is_gesture_event(int type, xcb_generic_event_t *event) {
    return (xinput_opcode != -1 && type == XCB_GE_GENERIC &&
            ((xcb_ge_generic_event_t *)event)->extension == xinput_opcode);
}

/*
 * Returns the XInput name of the given device, or NULL if it cannot be
 * determined.
 *
 */
static char *get_device_name(xcb_input_device_id_t deviceid) {
    xcb_input_xi_query_device_reply_t *reply = xcb_input_xi_query_device_reply(
        conn, xcb_input_xi_query_device(conn, deviceid), NULL);
    if (reply == NULL) {
        return NULL;
    }

    char *name = NULL;
    xcb_input_xi_device_info_iterator_t iter = xcb_input_xi_query_device_infos_iterator(reply);
    if (iter.rem > 0) {
        sasprintf(&name, "%.*s",
                  xcb_input_xi_device_info_name_length(iter.data),
                  xcb_input_xi_device_info_name(iter.data));
    }
    free(reply);
    return name;
}

static struct gesture_tracker *get_tracker(gesture_type_t type, xcb_input_device_id_t sourceid) {
    struct gesture_tracker *tracker;
    TAILQ_FOREACH (tracker, &gesture_trackers, trackers) {
        if (tracker->type == type && tracker->sourceid == sourceid) {
            return tracker;
        }
    }
    return NULL;
}

static void free_tracker(struct gesture_tracker *tracker) {
    TAILQ_REMOVE(&gesture_trackers, tracker, trackers);
    FREE(tracker->device_name);
    FREE(tracker);
}

/*
 * Derives the directions of a gesture from what has been gathered so far: one
 * of up, down, left or right from the summed displacement, and for pinches
 * additionally the turning and size directions.
 *
 */
static uint32_t gesture_tracker_directions(const struct gesture_tracker *tracker) {
    uint32_t directions = GESTURE_DIRECTION_NONE;

    if (fabs(tracker->dx) > fabs(tracker->dy)) {
        directions |= (tracker->dx > 0 ? GESTURE_DIRECTION_RIGHT : GESTURE_DIRECTION_LEFT);
    } else {
        directions |= (tracker->dy > 0 ? GESTURE_DIRECTION_DOWN : GESTURE_DIRECTION_UP);
    }

    if (tracker->type == GESTURE_TYPE_PINCH) {
        if (tracker->rotation > GESTURE_PINCH_ROTATION_DEGREES) {
            directions |= GESTURE_DIRECTION_CLOCKWISE;
        } else if (tracker->rotation < -GESTURE_PINCH_ROTATION_DEGREES) {
            directions |= GESTURE_DIRECTION_COUNTERCLOCKWISE;
        }

        if (tracker->scale > GESTURE_PINCH_OUTWARD_SCALE) {
            directions |= GESTURE_DIRECTION_OUTWARD;
        } else if (tracker->scale < GESTURE_PINCH_INWARD_SCALE) {
            directions |= GESTURE_DIRECTION_INWARD;
        }
    }

    return directions;
}

static int count_directions(uint32_t directions) {
    int n = 0;
    for (; directions != 0; directions &= (directions - 1)) {
        n++;
    }
    return n;
}

/*
 * Returns true if the gesture binding is triggered by the gesture (as far as it
 * has been gathered) with the given directions.
 *
 */
static bool gesture_binding_matches(const GestureBinding *bind, const struct gesture_tracker *tracker,
                                    uint32_t directions) {
    if (bind->type != tracker->type) {
        return false;
    }

    if (bind->fingers != 0 && bind->fingers != tracker->fingers) {
        return false;
    }

    if (bind->input_device != NULL &&
        (tracker->device_name == NULL || strcmp(bind->input_device, tracker->device_name) != 0)) {
        return false;
    }

    if (bind->exact) {
        return (bind->directions == directions);
    }
    return ((bind->directions & directions) == bind->directions);
}

/*
 * Compares the priority of two gesture bindings which are both triggered by
 * the same gesture. Returns a positive number if a is to be preferred over b,
 * a negative number if b is to be preferred over a and 0 if both are equally
 * specific.
 *
 */
static int gesture_binding_compare(const GestureBinding *a, const GestureBinding *b) {
    /* A binding for a specific device wins over a binding for all devices. */
    if ((a->input_device != NULL) != (b->input_device != NULL)) {
        return (a->input_device != NULL ? 1 : -1);
    }

    /* An exact binding wins over a non-exact binding. */
    if (a->exact != b->exact) {
        return (a->exact ? 1 : -1);
    }

    /* The binding which requires more directions wins. */
    return count_directions(a->directions) - count_directions(b->directions);
}

/*
 * Runs the best matching gesture binding of the current mode (if any) for the
 * gesture as gathered so far. When the gesture is still in progress (on an
 * update), only continuous bindings are considered; when it ends, all
 * bindings are.
 *
 */
static void gesture_tracker_run(const struct gesture_tracker *tracker, bool in_progress) {
    const uint32_t directions = gesture_tracker_directions(tracker);
    DLOG("Gesture %s: type %d, fingers %d, device \"%s\", dx %f, dy %f, scale %f, rotation %f, directions 0x%x\n",
         in_progress ? "updated" : "ended", tracker->type, tracker->fingers,
         tracker->device_name ? tracker->device_name : "(unknown)",
         tracker->dx, tracker->dy, tracker->scale, tracker->rotation, directions);

    GestureBinding *bind, *best = NULL;
    TAILQ_FOREACH (bind, gesture_bindings, gesture_bindings) {
        if (in_progress && !bind->continuous) {
            continue;
        }
        if (!gesture_binding_matches(bind, tracker, directions)) {
            continue;
        }
        /* On equal priority, the binding defined later in the configuration
         * wins. Whether a binding is continuous does not matter here. */
        if (best == NULL || gesture_binding_compare(bind, best) >= 0) {
            best = bind;
        }
    }

    if (best == NULL) {
        DLOG("No gesture binding matches.\n");
        return;
    }

    DLOG("Running gesture binding %s, command \"%s\"\n", best->symbol, best->command);
    Binding bind_for_run = {
        .input_type = B_GESTURE,
        .symbol = best->symbol,
        .command = best->command,
    };
    TAILQ_INIT(&(bind_for_run.keycodes_head));
    CommandResult *result = run_binding(&bind_for_run, NULL);
    command_result_free(result);
}

/*
 * Handles an XInput gesture event: tracks the gesture from its begin event to
 * its end event and runs the best matching gesture binding of the current
 * binding mode once the gesture ends. On every update of the gesture, the best
 * matching continuous gesture binding (if any) is run as well.
 *
 */
void handle_gesture_event(xcb_generic_event_t *event) {
    const uint16_t event_type = ((xcb_ge_generic_event_t *)event)->event_type;
    gesture_type_t type;
    xcb_input_device_id_t sourceid;
    uint32_t fingers, flags;
    xcb_input_fp1616_t delta_x, delta_y, scale = 0x10000, delta_angle = 0;

    switch (event_type) {
        case XCB_INPUT_GESTURE_PINCH_BEGIN:
        case XCB_INPUT_GESTURE_PINCH_UPDATE:
        case XCB_INPUT_GESTURE_PINCH_END: {
            const xcb_input_gesture_pinch_begin_event_t *pinch = (xcb_input_gesture_pinch_begin_event_t *)event;
            type = GESTURE_TYPE_PINCH;
            sourceid = pinch->sourceid;
            fingers = pinch->detail;
            flags = pinch->flags;
            delta_x = pinch->delta_x;
            delta_y = pinch->delta_y;
            scale = pinch->scale;
            delta_angle = pinch->delta_angle;
            break;
        }
        case XCB_INPUT_GESTURE_SWIPE_BEGIN:
        case XCB_INPUT_GESTURE_SWIPE_UPDATE:
        case XCB_INPUT_GESTURE_SWIPE_END: {
            const xcb_input_gesture_swipe_begin_event_t *swipe = (xcb_input_gesture_swipe_begin_event_t *)event;
            type = GESTURE_TYPE_SWIPE;
            sourceid = swipe->sourceid;
            fingers = swipe->detail;
            flags = swipe->flags;
            delta_x = swipe->delta_x;
            delta_y = swipe->delta_y;
            break;
        }
        default:
            return;
    }

    struct gesture_tracker *tracker = get_tracker(type, sourceid);
    switch (event_type) {
        case XCB_INPUT_GESTURE_PINCH_BEGIN:
        case XCB_INPUT_GESTURE_SWIPE_BEGIN:
            if (tracker != NULL) {
                /* We missed the end of the previous gesture. */
                free_tracker(tracker);
            }
            tracker = scalloc(1, sizeof(struct gesture_tracker));
            tracker->type = type;
            tracker->sourceid = sourceid;
            tracker->device_name = get_device_name(sourceid);
            tracker->fingers = fingers;
            tracker->scale = scale / 65536.0;
            TAILQ_INSERT_TAIL(&gesture_trackers, tracker, trackers);
            DLOG("Gesture began: type %d, fingers %d, device %d (\"%s\")\n",
                 type, fingers, sourceid, tracker->device_name ? tracker->device_name : "(unknown)");
            break;

        case XCB_INPUT_GESTURE_PINCH_UPDATE:
        case XCB_INPUT_GESTURE_SWIPE_UPDATE:
            if (tracker == NULL) {
                return;
            }
            tracker->dx += delta_x / 65536.0;
            tracker->dy += delta_y / 65536.0;
            if (type == GESTURE_TYPE_PINCH) {
                tracker->scale = scale / 65536.0;
                tracker->rotation += delta_angle / 65536.0;
            }
            /* The number of fingers and the device are those of the begin
             * event, even if an update reports a different number. */
            gesture_tracker_run(tracker, true);
            break;

        case XCB_INPUT_GESTURE_PINCH_END:
        case XCB_INPUT_GESTURE_SWIPE_END:
            if (tracker == NULL) {
                return;
            }
            /* Both cancelled flags have the same value. */
            if (flags & XCB_INPUT_GESTURE_SWIPE_EVENT_FLAGS_GESTURE_SWIPE_CANCELLED) {
                DLOG("Gesture was cancelled.\n");
            } else {
                gesture_tracker_run(tracker, false);
            }
            free_tracker(tracker);
            break;
    }
}

static void ungrab_gesture(uint8_t grab_type) {
    const uint32_t modifiers[] = {XCB_INPUT_MODIFIER_MASK_ANY};
    xcb_input_xi_passive_ungrab_device(conn, root, 0, XCB_INPUT_DEVICE_ALL_MASTER,
                                       1, grab_type, modifiers);
}

/*
 * Gestures are grabbed on the master devices, like key and button bindings
 * are grabbed on the core devices: each gesture is delivered once, with the
 * slave device it originates from as its source, and gestures of a floating
 * (detached) slave device are left alone.
 *
 */
static bool grab_gesture(uint8_t grab_type, int first_event) {
    /* Select the begin, update and end event of the gesture. */
    uint32_t mask[2] = {0, 0};
    for (int i = first_event; i < first_event + 3; i++) {
        mask[i / 32] |= (1u << (i % 32));
    }
    const uint32_t modifiers[] = {XCB_INPUT_MODIFIER_MASK_ANY};

    xcb_generic_error_t *error = NULL;
    xcb_input_xi_passive_grab_device_reply_t *reply = xcb_input_xi_passive_grab_device_reply(
        conn,
        xcb_input_xi_passive_grab_device(conn, XCB_CURRENT_TIME, root, XCB_NONE, 0,
                                         XCB_INPUT_DEVICE_ALL_MASTER, 1, 2, grab_type,
                                         XCB_INPUT_GRAB_MODE_22_ASYNC, XCB_INPUT_GRAB_MODE_22_ASYNC,
                                         false, mask, modifiers),
        &error);
    bool result = true;
    if (reply == NULL) {
        ELOG("Could not grab gestures (type %d), X11 error code %d\n", grab_type,
             error != NULL ? error->error_code : -1);
        result = false;
    } else if (reply->num_modifiers > 0) {
        ELOG("Could not grab gestures (type %d), another client might have grabbed them already.\n",
             grab_type);
        result = false;
    }
    free(reply);
    free(error);
    return result;
}

/*
 * (Re-)establishes the passive gesture grabs on the root window according to
 * the gesture bindings of the current binding mode: if the mode contains at
 * least one binding for swipes (pinches), all swipes (pinches) are grabbed,
 * otherwise they are passed on to the applications.
 *
 */
void gestures_grab(void) {
    if (xinput_opcode == -1) {
        return;
    }

    /* Hold gestures are not available through XInput, so hold bindings never
     * grab anything. */
    bool need_pinch = false;
    bool need_swipe = false;
    GestureBinding *bind;
    TAILQ_FOREACH (bind, gesture_bindings, gesture_bindings) {
        if (bind->type == GESTURE_TYPE_PINCH) {
            need_pinch = true;
        } else if (bind->type == GESTURE_TYPE_SWIPE) {
            need_swipe = true;
        }
    }
    DLOG("Grabbing gestures: pinch %d, swipe %d\n", need_pinch, need_swipe);

    if (pinch_grabbed) {
        ungrab_gesture(XCB_INPUT_GRAB_TYPE_GESTURE_PINCH_BEGIN);
        pinch_grabbed = false;
    }
    if (swipe_grabbed) {
        ungrab_gesture(XCB_INPUT_GRAB_TYPE_GESTURE_SWIPE_BEGIN);
        swipe_grabbed = false;
    }

    if (need_pinch) {
        pinch_grabbed = grab_gesture(XCB_INPUT_GRAB_TYPE_GESTURE_PINCH_BEGIN, XCB_INPUT_GESTURE_PINCH_BEGIN);
    }
    if (need_swipe) {
        swipe_grabbed = grab_gesture(XCB_INPUT_GRAB_TYPE_GESTURE_SWIPE_BEGIN, XCB_INPUT_GESTURE_SWIPE_BEGIN);
    }
}
