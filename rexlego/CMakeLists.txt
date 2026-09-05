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
    src/updates.cpp
)

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
# guest CPU time goes. The SDK presets only set -march for ARM; on x86 the
# recompiled code was built for a baseline x86-64 without AVX2/BMI2/FMA.
# TheSimpsonsGameRecomp measured this + -O3 as a real win. x86-64-v3 is
# Haswell/Zen1+; our 5700X3D is fine. Applied to both targets so the app's own
# code and the recomp library agree.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(legodimensions PRIVATE -march=x86-64-v3)
    if(TARGET legodimensions_recomp)
        target_compile_options(legodimensions_recomp PRIVATE
            $<$<CONFIG:RelWithDebInfo,Release>:-O3>
            -march=x86-64-v3)
    endif()
endif()
