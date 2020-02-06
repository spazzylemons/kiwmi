/* Copyright (c), Niclas Meyer <niclas@countingsort.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "luak/kiwmi_server.h"

#include <stdlib.h>

#include <unistd.h>

#include <lauxlib.h>
#include <wayland-server.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/log.h>

#include "desktop/view.h"
#include "input/cursor.h"
#include "input/input.h"
#include "input/seat.h"
#include "luak/kiwmi_cursor.h"
#include "luak/kiwmi_keyboard.h"
#include "luak/kiwmi_lua_callback.h"
#include "luak/kiwmi_output.h"
#include "luak/kiwmi_view.h"
#include "server.h"

static int
l_kiwmi_server_cursor(lua_State *L)
{
    struct kiwmi_object *obj =
        *(struct kiwmi_object **)luaL_checkudata(L, 1, "kiwmi_server");

    struct kiwmi_server *server = obj->object;

    lua_pushcfunction(L, luaK_kiwmi_cursor_new);
    lua_pushlightuserdata(L, server->lua);
    lua_pushlightuserdata(L, server->input.cursor);
    if (lua_pcall(L, 2, 1, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        return 0;
    }

    return 1;
}

static int
l_kiwmi_server_focused_view(lua_State *L)
{
    struct kiwmi_object *obj =
        *(struct kiwmi_object **)luaL_checkudata(L, 1, "kiwmi_server");

    struct kiwmi_server *server = obj->object;

    if (!server->input.seat->focused_view) {
        return 0;
    }

    lua_pushcfunction(L, luaK_kiwmi_view_new);
    lua_pushlightuserdata(L, server->lua);
    lua_pushlightuserdata(L, server->input.seat->focused_view);
    if (lua_pcall(L, 2, 1, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        return 0;
    }

    return 1;
}

static int
l_kiwmi_server_quit(lua_State *L)
{
    struct kiwmi_object *obj =
        *(struct kiwmi_object **)luaL_checkudata(L, 1, "kiwmi_server");

    struct kiwmi_server *server = obj->object;

    wl_display_terminate(server->wl_display);

    return 0;
}

static int
l_kiwmi_server_spawn(lua_State *L)
{
    luaL_checkudata(L, 1, "kiwmi_server");
    luaL_checktype(L, 2, LUA_TSTRING);

    const char *command = lua_tostring(L, 2);

    pid_t pid = fork();

    if (pid < 0) {
        return luaL_error(L, "Failed to run command (fork)");
    }

    if (pid == 0) {
        execl("/bin/sh", "/bin/sh", "-c", command, NULL);
        _exit(EXIT_FAILURE);
    }

    lua_pushinteger(L, pid);

    return 1;
}

static int
l_kiwmi_server_stop_interactive(lua_State *L)
{
    struct kiwmi_object *obj =
        *(struct kiwmi_object **)luaL_checkudata(L, 1, "kiwmi_server");

    struct kiwmi_server *server = obj->object;

    server->input.cursor->cursor_mode = KIWMI_CURSOR_PASSTHROUGH;

    return 0;
}

static int
l_kiwmi_server_view_at(lua_State *L)
{
    struct kiwmi_object *obj =
        *(struct kiwmi_object **)luaL_checkudata(L, 1, "kiwmi_server");
    luaL_checktype(L, 2, LUA_TNUMBER); // x
    luaL_checktype(L, 3, LUA_TNUMBER); // y

    struct kiwmi_server *server = obj->object;

    double x = lua_tonumber(L, 2);
    double y = lua_tonumber(L, 3);

    struct wlr_surface *surface;
    double sx;
    double sy;

    struct kiwmi_view *view =
        view_at(&server->desktop, x, y, &surface, &sx, &sy);

    if (view) {
        lua_pushcfunction(L, luaK_kiwmi_view_new);
        lua_pushlightuserdata(L, obj->lua);
        lua_pushlightuserdata(L, view);
        if (lua_pcall(L, 2, 1, 0)) {
            wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
            return 0;
        }
    } else {
        lua_pushnil(L);
    }

    return 1;
}

static const luaL_Reg kiwmi_server_methods[] = {
    {"cursor", l_kiwmi_server_cursor},
    {"focused_view", l_kiwmi_server_focused_view},
    {"on", luaK_callback_register_dispatch},
    {"quit", l_kiwmi_server_quit},
    {"spawn", l_kiwmi_server_spawn},
    {"stop_interactive", l_kiwmi_server_stop_interactive},
    {"view_at", l_kiwmi_server_view_at},
    {NULL, NULL},
};

static void
kiwmi_server_on_keyboard_notify(struct wl_listener *listener, void *data)
{
    struct kiwmi_lua_callback *lc   = wl_container_of(listener, lc, listener);
    struct kiwmi_server *server     = lc->server;
    lua_State *L                    = server->lua->L;
    struct kiwmi_keyboard *keyboard = data;

    lua_rawgeti(L, LUA_REGISTRYINDEX, lc->callback_ref);

    lua_pushcfunction(L, luaK_kiwmi_keyboard_new);
    lua_pushlightuserdata(L, server->lua);
    lua_pushlightuserdata(L, keyboard);
    if (lua_pcall(L, 2, 1, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    if (lua_pcall(L, 1, 0, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void
kiwmi_server_on_output_notify(struct wl_listener *listener, void *data)
{
    struct kiwmi_lua_callback *lc = wl_container_of(listener, lc, listener);
    struct kiwmi_server *server   = lc->server;
    lua_State *L                  = server->lua->L;
    struct kiwmi_output *output   = data;

    lua_rawgeti(L, LUA_REGISTRYINDEX, lc->callback_ref);

    lua_pushcfunction(L, luaK_kiwmi_output_new);
    lua_pushlightuserdata(L, server->lua);
    lua_pushlightuserdata(L, output);
    if (lua_pcall(L, 2, 1, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    if (lua_pcall(L, 1, 0, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void
kiwmi_server_on_view_notify(struct wl_listener *listener, void *data)
{
    struct kiwmi_lua_callback *lc = wl_container_of(listener, lc, listener);
    struct kiwmi_server *server   = lc->server;
    lua_State *L                  = server->lua->L;
    struct kiwmi_view *view       = data;

    lua_rawgeti(L, LUA_REGISTRYINDEX, lc->callback_ref);

    lua_pushcfunction(L, luaK_kiwmi_view_new);
    lua_pushlightuserdata(L, server->lua);
    lua_pushlightuserdata(L, view);
    if (lua_pcall(L, 2, 1, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    if (lua_pcall(L, 1, 0, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static int
l_kiwmi_server_on_keyboard(lua_State *L)
{
    struct kiwmi_object *obj =
        *(struct kiwmi_object **)luaL_checkudata(L, 1, "kiwmi_server");
    luaL_checktype(L, 2, LUA_TFUNCTION);

    struct kiwmi_server *server = obj->object;

    lua_pushcfunction(L, luaK_kiwmi_lua_callback_new);
    lua_pushlightuserdata(L, server);
    lua_pushvalue(L, 2);
    lua_pushlightuserdata(L, kiwmi_server_on_keyboard_notify);
    lua_pushlightuserdata(L, &server->input.events.keyboard_new);
    lua_pushlightuserdata(L, obj);

    if (lua_pcall(L, 5, 0, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        return 0;
    }

    return 0;
}

static int
l_kiwmi_server_on_output(lua_State *L)
{
    struct kiwmi_object *obj =
        *(struct kiwmi_object **)luaL_checkudata(L, 1, "kiwmi_server");
    luaL_checktype(L, 2, LUA_TFUNCTION);

    struct kiwmi_server *server = obj->object;

    lua_pushcfunction(L, luaK_kiwmi_lua_callback_new);
    lua_pushlightuserdata(L, server);
    lua_pushvalue(L, 2);
    lua_pushlightuserdata(L, kiwmi_server_on_output_notify);
    lua_pushlightuserdata(L, &server->desktop.events.new_output);
    lua_pushlightuserdata(L, obj);

    if (lua_pcall(L, 5, 0, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        return 0;
    }

    return 0;
}

static int
l_kiwmi_server_on_view(lua_State *L)
{
    struct kiwmi_object *obj =
        *(struct kiwmi_object **)luaL_checkudata(L, 1, "kiwmi_server");
    luaL_checktype(L, 2, LUA_TFUNCTION);

    struct kiwmi_server *server = obj->object;

    lua_pushcfunction(L, luaK_kiwmi_lua_callback_new);
    lua_pushlightuserdata(L, server);
    lua_pushvalue(L, 2);
    lua_pushlightuserdata(L, kiwmi_server_on_view_notify);
    lua_pushlightuserdata(L, &server->desktop.events.view_map);
    lua_pushlightuserdata(L, obj);

    if (lua_pcall(L, 5, 0, 0)) {
        wlr_log(WLR_ERROR, "%s", lua_tostring(L, -1));
        return 0;
    }

    return 0;
}

static const luaL_Reg kiwmi_server_events[] = {
    {"keyboard", l_kiwmi_server_on_keyboard},
    {"output", l_kiwmi_server_on_output},
    {"view", l_kiwmi_server_on_view},
    {NULL, NULL},
};

int
luaK_kiwmi_server_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TLIGHTUSERDATA); // kiwmi_lua
    luaL_checktype(L, 2, LUA_TLIGHTUSERDATA); // kiwmi_server

    struct kiwmi_lua *lua       = lua_touserdata(L, 1);
    struct kiwmi_server *server = lua_touserdata(L, 2);

    struct kiwmi_object *obj = luaK_get_kiwmi_object(lua, server, NULL);

    struct kiwmi_object **server_ud = lua_newuserdata(L, sizeof(*server_ud));
    luaL_getmetatable(L, "kiwmi_server");
    lua_setmetatable(L, -2);

    *server_ud = obj;

    return 1;
}

int
luaK_kiwmi_server_register(lua_State *L)
{
    luaL_newmetatable(L, "kiwmi_server");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, kiwmi_server_methods, 0);

    luaL_newlib(L, kiwmi_server_events);
    lua_setfield(L, -2, "__events");

    lua_pushcfunction(L, luaK_usertype_ref_equal);
    lua_setfield(L, -2, "__eq");

    lua_pushcfunction(L, luaK_kiwmi_object_gc);
    lua_setfield(L, -2, "__gc");

    return 0;
}
