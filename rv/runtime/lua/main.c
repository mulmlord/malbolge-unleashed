// The Lua interpreter for `malm build script.lua` (docs/rv.md 6, "Lua"): runs the script linked
// with it (script.S) as the stand-alone `lua script.lua` does, without its options, LUA_INIT and
// script arguments: the standard libraries, the global `arg` with the script's name at index 0,
// the generational collector, and an error message with a traceback on stderr (`lua: ...`, status
// 1).
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

// From script.S: the script's text and its chunk name, "@" and the script's file name.
extern const char malm_lua_script[];
extern const char malm_lua_script_end[];
extern const char malm_lua_name[];

// lua.c's: the error message with a traceback, or for an error object that is not a string its
// __tostring or its type.
static int msghandler(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (msg == NULL) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) {
            return 1;
        }
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// In protected mode: an error, the script's or a syntax error, propagates to main.
static int pmain(lua_State* L) {
    luaL_openlibs(L);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, malm_lua_name + 1);
    lua_rawseti(L, -2, 0);
    lua_setglobal(L, "arg");
    lua_gc(L, LUA_GCRESTART);
    lua_gc(L, LUA_GCGEN);
    lua_pushcfunction(L, msghandler);
    const size_t size = (size_t)(malm_lua_script_end - malm_lua_script);
    int status = luaL_loadbuffer(L, malm_lua_script, size, malm_lua_name);
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, 0, -2);
    }
    if (status != LUA_OK) {
        return lua_error(L);
    }
    return 0;
}

int main(void) {
    lua_State* L = luaL_newstate();
    if (L == NULL) {
        fputs("lua: cannot create state: not enough memory\n", stderr);
        return EXIT_FAILURE;
    }
    lua_gc(L, LUA_GCSTOP); // while the libraries are opened, as lua.c does
    lua_pushcfunction(L, pmain);
    const int status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        fprintf(stderr, "lua: %s\n", msg != NULL ? msg : "(error message not a string)");
    }
    lua_close(L);
    return status == LUA_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
