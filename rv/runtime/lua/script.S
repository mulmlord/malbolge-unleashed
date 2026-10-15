# The script of a Lua program for the interpreter (main.c, docs/rv.md 6): `malm build` assembles
# this with -I naming a directory that holds the script as script.lua and its chunk name ("@" and
# the script's file name) as script.name.
    .section .rodata.malm_lua_script, "a", @progbits
    .globl malm_lua_script, malm_lua_script_end, malm_lua_name
malm_lua_script:
    .incbin "script.lua"
malm_lua_script_end:
malm_lua_name:
    .incbin "script.name"
    .byte 0
