/* Copyright (c), Niclas Meyer <niclas@countingsort.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "desktop/xwayland.h"

#include <stdlib.h>

#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "input/cursor.h"
#include "input/seat.h"
#include "server.h"

struct xwayland_iterator_data {
    wlr_surface_iterator_func_t user_iterator;
    void *user_data;
    int x, y;
};

static void
xwayland_view_close(struct kiwmi_view *view)
{
    wlr_xwayland_surface_close(view->xwayland_surface);
}

static void
xwayland_iterator(struct wlr_surface *surface, int sx, int sy, void *data)
{
    struct xwayland_iterator_data *iter_data = data;
    iter_data->user_iterator(
        surface, iter_data->x + sx, iter_data->y + sy, iter_data->user_data);
}

static void
for_each_children(
    struct wlr_xwayland_surface *surface,
    int x,
    int y,
    wlr_surface_iterator_func_t iterator,
    void *user_data)
{
    struct wlr_xwayland_surface *child;
    wl_list_for_each (child, &surface->children, link) {
        if (!child->mapped) {
            continue;
        }

        struct xwayland_iterator_data data = {
            .user_iterator = iterator,
            .user_data     = user_data,
            .x             = x + child->x,
            .y             = y + child->y,
        };
        wlr_surface_for_each_surface(child->surface, xwayland_iterator, &data);

        for_each_children(
            child, x + child->x, y + child->y, iterator, user_data);
    }
}

static void
xwayland_view_for_each_surface(
    struct kiwmi_view *view,
    wlr_surface_iterator_func_t callback,
    void *user_data)
{
    wlr_surface_for_each_surface(
        view->xwayland_surface->surface, callback, user_data);
    for_each_children(view->xwayland_surface, 0, 0, callback, user_data);
}

static pid_t
xwayland_view_get_pid(struct kiwmi_view *view)
{
    return view->xwayland_surface->pid;
}

static const char *
xwayland_view_get_string_prop(
    struct kiwmi_view *view,
    enum kiwmi_view_prop prop)
{
    switch (prop) {
    case KIWMI_VIEW_PROP_APP_ID:
        return view->xwayland_surface->class;
    case KIWMI_VIEW_PROP_TITLE:
        return view->xwayland_surface->title;
    default:
        return NULL;
    }
}

static void
xwayland_view_set_activated(struct kiwmi_view *view, bool activated)
{
    wlr_xwayland_surface_activate(view->xwayland_surface, activated);
}

static void
xwayland_view_set_size(struct kiwmi_view *view, uint32_t width, uint32_t height)
{
    if (width > UINT16_MAX || height > UINT16_MAX) {
        return;
    }
    wlr_xwayland_surface_configure(
        view->xwayland_surface,
        view->xwayland_surface->x,
        view->xwayland_surface->y,
        width,
        height);
}

static void
xwayland_view_set_tiled(
    struct kiwmi_view *UNUSED(view),
    enum wlr_edges UNUSED(edges))
{
    /** TODO: implement tiling for xwayland */
    wlr_log(WLR_ERROR, "Xwayland tiling support not implemented");
}

static struct wlr_surface *
locate_surface(
    struct wlr_xwayland_surface *current,
    double sx,
    double sy,
    double *sub_x,
    double *sub_y)
{
    struct wlr_xwayland_surface *child;
    wl_list_for_each (child, &current->children, link) {
        if (!child->mapped)
            continue;

        double child_sx = current->x + child->x;
        double child_sy = current->y + child->y;

        struct wlr_surface *result =
            locate_surface(child, sx - child_sx, sy - child_sy, sub_x, sub_y);
        if (result) {
            return result;
        }
    }

    return wlr_surface_surface_at(current->surface, sx, sy, sub_x, sub_y);
}

struct wlr_surface *
xwayland_view_surface_at(
    struct kiwmi_view *view,
    double sx,
    double sy,
    double *sub_x,
    double *sub_y)
{
    return locate_surface(view->xwayland_surface, sx, sy, sub_x, sub_y);
}

static const struct kiwmi_view_impl xwayland_view_impl = {
    .close            = xwayland_view_close,
    .for_each_surface = xwayland_view_for_each_surface,
    .get_pid          = xwayland_view_get_pid,
    .get_string_prop  = xwayland_view_get_string_prop,
    .set_activated    = xwayland_view_set_activated,
    .set_size         = xwayland_view_set_size,
    .set_tiled        = xwayland_view_set_tiled,
    .surface_at       = xwayland_view_surface_at,
};

static void
xwayland_commit_notify(struct wl_listener *listener, void *UNUSED(data))
{
    struct kiwmi_view *view = wl_container_of(listener, view, commit);

    struct kiwmi_desktop *desktop = view->desktop;
    struct kiwmi_server *server   = wl_container_of(desktop, server, desktop);
    struct kiwmi_cursor *cursor   = server->input.cursor;
    cursor_refresh_focus(cursor, NULL, NULL, NULL);

    if (pixman_region32_not_empty(&view->wlr_surface->buffer_damage)) {
        struct kiwmi_output *output;
        wl_list_for_each (output, &desktop->outputs, link) {
            output_damage(output);
        }
    }

    wlr_surface_get_extends(view->wlr_surface, &view->geom);

    const struct wlr_box surface_geom = {
        .x      = view->xwayland_surface->x,
        .y      = view->xwayland_surface->y,
        .width  = view->xwayland_surface->width,
        .height = view->xwayland_surface->height,
    };

    wlr_box_intersection(&view->geom, &surface_geom, &view->geom);
}

static void
xwayland_destroy_notify(struct wl_listener *listener, void *UNUSED(data))
{
    struct kiwmi_view *view       = wl_container_of(listener, view, destroy);
    struct kiwmi_desktop *desktop = view->desktop;
    struct kiwmi_server *server   = wl_container_of(desktop, server, desktop);
    struct kiwmi_seat *seat       = server->input.seat;

    if (seat->focused_view == view) {
        seat->focused_view = NULL;
    }
    cursor_refresh_focus(server->input.cursor, NULL, NULL, NULL);

    struct kiwmi_view_child *child, *tmpchild;
    wl_list_for_each_safe (child, tmpchild, &view->children, link) {
        child->mapped = false;
        view_child_destroy(child);
    }

    wl_list_remove(&view->link);
    wl_list_remove(&view->children);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->new_subsurface.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);

    wl_list_remove(&view->events.unmap.listener_list);

    free(view);
}

/** NOTE: identical to xdg_surface_new_subsurface_notify */
static void
xwayland_new_subsurface_notify(struct wl_listener *listener, void *data)
{
    struct kiwmi_view *view = wl_container_of(listener, view, new_subsurface);
    struct wlr_subsurface *subsurface = data;
    view_child_subsurface_create(NULL, view, subsurface);
}

static void
xwayland_map_notify(struct wl_listener *listener, void *UNUSED(data))
{
    struct kiwmi_view *view = wl_container_of(listener, view, map);
    view->mapped            = true;

    if (view->wlr_surface != NULL) {
        return;
    }

    view->wlr_surface = view->xwayland_surface->surface;

    wlr_log(
        WLR_DEBUG,
        "New Xwayland surface title='%s' class='%s'",
        view->xwayland_surface->title,
        view->xwayland_surface->class);

    view->commit.notify = xwayland_commit_notify;
    wl_signal_add(&view->wlr_surface->events.commit, &view->commit);

    view->destroy.notify = xwayland_destroy_notify;
    wl_signal_add(&view->wlr_surface->events.destroy, &view->destroy);

    view->new_subsurface.notify = xwayland_new_subsurface_notify;
    wl_signal_add(
        &view->wlr_surface->events.new_subsurface, &view->new_subsurface);

    view_init_subsurfaces(NULL, view);

    wl_list_insert(&view->desktop->views, &view->link);

    struct kiwmi_output *output;
    wl_list_for_each (output, &view->desktop->outputs, link) {
        output_damage(output);
    }

    wl_signal_emit(&view->desktop->events.view_map, view);
}

/** NOTE: identical to xdg_surface_unmap_notify */
static void
xwayland_unmap_notify(struct wl_listener *listener, void *UNUSED(data))
{
    struct kiwmi_view *view = wl_container_of(listener, view, unmap);

    if (view->mapped) {
        view->mapped = false;

        struct kiwmi_output *output;
        wl_list_for_each (output, &view->desktop->outputs, link) {
            output_damage(output);
        }

        wl_signal_emit(&view->events.unmap, view);
    }
}

/** NOTE: identical to xdg_toplevel_request_move_notify */
static void
xwayland_request_move_notify(struct wl_listener *listener, void *UNUSED(data))
{
    struct kiwmi_view *view = wl_container_of(listener, view, request_move);

    wl_signal_emit(&view->events.request_move, view);
}

static void
xwayland_request_resize_notify(struct wl_listener *listener, void *data)
{
    struct kiwmi_view *view = wl_container_of(listener, view, request_resize);
    struct wlr_xwayland_resize_event *event = data;

    struct kiwmi_request_resize_event new_event = {
        .view  = view,
        .edges = event->edges,
    };

    wl_signal_emit(&view->events.request_resize, &new_event);
}

static void
xwayland_new_surface_notify(struct wl_listener *listener, void *data)
{
    struct kiwmi_desktop *desktop =
        wl_container_of(listener, desktop, xwayland_new_surface);
    struct wlr_xwayland_surface *xwayland_surface = data;

    wlr_xwayland_surface_ping(xwayland_surface);

    struct kiwmi_view *view =
        view_create(desktop, KIWMI_VIEW_XWAYLAND, &xwayland_view_impl);
    if (!view) {
        return;
    }

    xwayland_surface->data = view;

    view->xwayland_surface = xwayland_surface;
    view->wlr_surface      = NULL;

    view->map.notify = xwayland_map_notify;
    wl_signal_add(&xwayland_surface->events.map, &view->map);

    view->unmap.notify = xwayland_unmap_notify;
    wl_signal_add(&xwayland_surface->events.unmap, &view->unmap);

    view->request_move.notify = xwayland_request_move_notify;
    wl_signal_add(&xwayland_surface->events.request_move, &view->request_move);

    view->request_resize.notify = xwayland_request_resize_notify;
    wl_signal_add(
        &xwayland_surface->events.request_resize, &view->request_resize);
}

bool
xwayland_init(struct kiwmi_desktop *desktop)
{
    struct kiwmi_server *server = wl_container_of(desktop, server, desktop);
    desktop->xwayland =
        wlr_xwayland_create(server->wl_display, desktop->compositor, false);

    if (!desktop->xwayland) {
        return false;
    }

    if (!wlr_xcursor_manager_load(
            server->input.cursor->xcursor_manager, 1.0f)) {
        wlr_xwayland_destroy(desktop->xwayland);
        return false;
    }

    struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(
        server->input.cursor->xcursor_manager, "left_ptr", 1.0f);
    if (xcursor) {
        struct wlr_xcursor_image *image = xcursor->images[0];
        wlr_xwayland_set_cursor(
            desktop->xwayland,
            image->buffer,
            image->width * 4,
            image->width,
            image->height,
            image->hotspot_x,
            image->hotspot_y);
    }

    wlr_log(
        WLR_DEBUG,
        "Started Xwayland on display '%s'",
        desktop->xwayland->display_name);

    wlr_xwayland_set_seat(desktop->xwayland, server->input.seat->seat);

    setenv("DISPLAY", desktop->xwayland->display_name, true);

    desktop->xwayland_new_surface.notify = xwayland_new_surface_notify;
    wl_signal_add(
        &desktop->xwayland->events.new_surface, &desktop->xwayland_new_surface);

    return true;
}

void
xwayland_fini(struct kiwmi_desktop *desktop)
{
    wlr_xwayland_destroy(desktop->xwayland);
}
