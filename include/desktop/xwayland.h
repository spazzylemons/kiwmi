/* Copyright (c), Niclas Meyer <niclas@countingsort.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef KIWMI_DESKTOP_XWAYLAND_H
#define KIWMI_DESKTOP_XWAYLAND_H

#include "desktop/desktop.h"

bool xwayland_init(struct kiwmi_desktop *desktop);
void xwayland_fini(struct kiwmi_desktop *desktop);

#endif /* KIWMI_DESKTOP_XWAYLAND_H */
