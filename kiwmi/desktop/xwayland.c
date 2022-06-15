/* Copyright (c), Niclas Meyer <niclas@countingsort.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "desktop/xwayland.h"

#include <stdlib.h>

#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "input/seat.h"
#include "server.h"

bool
xwayland_init(struct kiwmi_desktop *desktop)
{
    struct kiwmi_server *server = wl_container_of(desktop, server, desktop);
    desktop->xwayland =
        wlr_xwayland_create(server->wl_display, desktop->compositor, false);

    if (!desktop->xwayland) {
        return false;
    }

    wlr_log(
        WLR_DEBUG,
        "Started Xwayland on display '%s'",
        desktop->xwayland->display_name);

    wlr_xwayland_set_seat(desktop->xwayland, server->input.seat->seat);

    setenv("DISPLAY", desktop->xwayland->display_name, true);

    return true;
}

void
xwayland_fini(struct kiwmi_desktop *desktop)
{
    wlr_xwayland_destroy(desktop->xwayland);
}
