# Lua for `malm build script.lua` (docs/rv.md 6), cross-compiled by rv/runtime/CMakeLists.txt with
# riscv32.cmake into the picolibc sysroot: liblua.a, Lua's core and standard libraries and main.c,
# the interpreter that runs the script linked with it (script.S, which malm build assembles).
# Lua's ISO C configuration: no dynamic libraries, no popen, no POSIX extras.
cmake_minimum_required(VERSION 3.28)
project(malm_lua LANGUAGES C)

set(SYSROOT "" CACHE PATH "The sysroot: picolibc installed there, and this library")
set(LUA_DIR "" CACHE PATH "Lua's source")
option(WERROR "Treat warnings as errors" OFF)

# Lua's src/Makefile: CORE_O and LIB_O.
set(lua_sources lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser
    lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib
    loadlib loslib lstrlib ltablib lutf8lib linit)
list(TRANSFORM lua_sources PREPEND "${LUA_DIR}/src/")
list(TRANSFORM lua_sources APPEND ".c")

add_library(lua STATIC ${lua_sources} main.c)
target_include_directories(lua PRIVATE "${LUA_DIR}/src")
target_compile_options(lua PRIVATE -nostdlibinc -isystem "${SYSROOT}/include" -ffunction-sections
    -fdata-sections -std=gnu99 -Wall -Wextra)
set_source_files_properties(main.c PROPERTIES COMPILE_OPTIONS
    "-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;$<$<BOOL:${WERROR}>:-Werror>")

install(TARGETS lua ARCHIVE DESTINATION lib)
