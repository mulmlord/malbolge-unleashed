# rv-emu (docs/rv.md 8): the RV32IM emulator. `malm rvemu` generates its HLA program (emu.mhl) and
# `malm hla` compiles it to emu.malm, with MALM_EMU_RAM bytes of guest RAM. Every change to malm
# rebuilds it: about 25 seconds for 1M in a release build. A sanitized debug build needs about
# 3 minutes for 4K and some 20 for 1M, so the debug preset turns MALM_BUILD_EMU off. Running it
# needs about 5 GB of host memory with 1M (docs/rv.md 8).
option(MALM_BUILD_EMU "Build rv-emu's emu.malm with the toolchain" ON)
set(MALM_EMU_RAM 1M CACHE STRING "Guest RAM of emu.malm: bytes, or with a K or M suffix (at most 4M)")
if(NOT MALM_BUILD_EMU)
    return()
endif()

set(emu_mhl "${CMAKE_CURRENT_BINARY_DIR}/emu.mhl")
set(emu_malm "${CMAKE_CURRENT_BINARY_DIR}/emu.malm")
add_custom_command(OUTPUT "${emu_mhl}" "${emu_malm}"
    COMMAND malm rvemu --ram ${MALM_EMU_RAM} -o "${emu_mhl}"
    COMMAND malm hla -o "${emu_malm}" "${emu_mhl}"
    DEPENDS malm
    COMMENT "Building rv-emu emu.malm (guest RAM ${MALM_EMU_RAM})"
    VERBATIM)
add_custom_target(emu ALL DEPENDS "${emu_malm}")
