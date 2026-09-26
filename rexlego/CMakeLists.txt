# legodimensions - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
# RC compiles res/legodimensions.rc, which carries the executable's icon.
# Enabled on Windows only; the .rc is left out of the sources elsewhere.
if(WIN32)
    project(legodimensions LANGUAGES CXX RC)
else()
    project(legodimensions LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(LEGODIMENSIONS_SOURCES
    src/main.cpp
    src/discord_presence.cpp
    src/ui_theme.cpp
    src/mod_menu.cpp
    src/main_menu_extras.cpp
    src/cheat_engine.cpp
    src/cheat_menu.cpp
    src/toypad_app.cpp
    src/updates.cpp
    src/name_trace.cpp
)

# Research probes: a guard-page watchpoint on the area flags and the failed
# attempt at forcing the 3D portrait. They are off in a release because one of
# them single-steps every read of a watched page, which is slow enough to look
# like a hang, and neither is any use to a player. Turn them back on with
# -DLEGODIMENSIONS_DEV_PROBES=ON while working on the portrait.
option(LEGODIMENSIONS_DEV_PROBES "Build the area-flag watchpoint and portrait experiments" OFF)
if(LEGODIMENSIONS_DEV_PROBES)
    list(APPEND LEGODIMENSIONS_SOURCES src/icon_probe.cpp src/hub_portrait.cpp
        src/area_watch.cpp)
    add_compile_definitions(LEGODIMENSIONS_DEV_PROBES)
endif()

if(WIN32)
    add_executable(legodimensions WIN32 ${LEGODIMENSIONS_SOURCES}
                   res/legodimensions.rc)
else()
    add_executable(legodimensions ${LEGODIMENSIONS_SOURCES})
endif()

rexglue_setup_target(legodimensions)

# The achievement unlock sound, converted from the user-supplied MP3 to a quiet
# 48 kHz WAV. Copied next to the executable because that is where the SDK cvar
# ui_sound_achievement looks by default; point the cvar elsewhere to swap it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/achievement_unlocked.wav")
    add_custom_command(TARGET legodimensions POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/res/achievement_unlocked.wav"
                "$<TARGET_FILE_DIR:legodimensions>/achievement_unlocked.wav"
        COMMENT "Copying achievement sound next to the executable")
endif()

# The generated translation units (legodimensions_recomp) are where nearly all
# guest CPU time goes, and they were being built for a bare x86-64 without
# AVX2/BMI2/FMA. TheSimpsonsGameRecomp measured this + -O3 as a real win, so
# the shipped builds use x86-64-v3 (Haswell / Zen 1 or newer).
#
# It is a cache variable rather than a hard-coded flag because people do build
# this for older CPUs. Passing -DCMAKE_CXX_FLAGS=-march=... does NOT work for
# that: target_compile_options land after CMAKE_CXX_FLAGS on the command line,
# so the value here would win. Lower the floor with:
#
#   cmake -S rexlego -B ... -DLEGODIMENSIONS_X86_BASELINE=x86-64-v2
#
# x86-64-v2 (Nehalem/Bulldozer) is the lowest that builds - the SDK's byte-swap
# helpers need SSSE3. Set it to "none" to supply your own -march in
# CMAKE_CXX_FLAGS. Whatever is chosen here must also be passed to the SDK
# (-DREXGLUE_X86_BASELINE=...), or the two halves disagree about AVX2 and the
# game crashes with an illegal instruction on the CPU that cannot run it.
set(LEGODIMENSIONS_X86_BASELINE "x86-64-v3" CACHE STRING
    "x86 -march baseline for the game and the recompiled code (or \"none\")")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    if(LEGODIMENSIONS_X86_BASELINE AND NOT LEGODIMENSIONS_X86_BASELINE STREQUAL "none")
        message(STATUS "legodimensions x86 baseline: -march=${LEGODIMENSIONS_X86_BASELINE}")
        target_compile_options(legodimensions PRIVATE -march=${LEGODIMENSIONS_X86_BASELINE})
    endif()
    if(TARGET legodimensions_recomp)
        target_compile_options(legodimensions_recomp PRIVATE
            $<$<CONFIG:RelWithDebInfo,Release>:-O3>)
        if(LEGODIMENSIONS_X86_BASELINE AND NOT LEGODIMENSIONS_X86_BASELINE STREQUAL "none")
            target_compile_options(legodimensions_recomp PRIVATE
                -march=${LEGODIMENSIONS_X86_BASELINE})
        endif()
    endif()
endif()
