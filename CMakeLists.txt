cmake_minimum_required(VERSION 3.24)
project(quake2 LANGUAGES C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This port is 64-bit only. Configure a 64-bit toolchain.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# SDL3 — prefer an installed package, otherwise fetch and build from source
# ---------------------------------------------------------------------------
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL
        GIT_TAG main
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endif()

# ---------------------------------------------------------------------------
# Common compile options
# ---------------------------------------------------------------------------
add_library(q2_common_flags INTERFACE)
if(MSVC)
    target_compile_definitions(q2_common_flags INTERFACE
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE C_ONLY)
    target_compile_options(q2_common_flags INTERFACE /W3)
else()
    target_compile_definitions(q2_common_flags INTERFACE C_ONLY)
    target_compile_options(q2_common_flags INTERFACE
        -fno-strict-aliasing -fwrapv
        -Wall -Wno-unused-parameter -Wno-sign-compare
        -Wno-unused-but-set-variable)
endif()

# ---------------------------------------------------------------------------
# q2ded — dedicated server
# ---------------------------------------------------------------------------
set(QCOMMON_SOURCES
    src/qcommon/cmd.c
    src/qcommon/cmodel.c
    src/qcommon/common.c
    src/qcommon/crc.c
    src/qcommon/cvar.c
    src/qcommon/files.c
    src/qcommon/md4.c
    src/qcommon/net_chan.c
    src/qcommon/pmove.c
    src/game/q_shared.c
)

set(SERVER_SOURCES
    src/server/sv_ccmds.c
    src/server/sv_ents.c
    src/server/sv_game.c
    src/server/sv_init.c
    src/server/sv_main.c
    src/server/sv_send.c
    src/server/sv_user.c
    src/server/sv_world.c
)

add_executable(q2ded
    ${QCOMMON_SOURCES}
    ${SERVER_SOURCES}
    src/null/cl_null.c
    src/null/cd_null.c
    src/sdl/sys_sdl.c
    src/sdl/net_sockets.c
)
target_compile_definitions(q2ded PRIVATE DEDICATED_ONLY)
target_include_directories(q2ded PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(q2ded PRIVATE q2_common_flags SDL3::SDL3)
if(WIN32)
    target_link_libraries(q2ded PRIVATE ws2_32)
endif()
set_target_properties(q2ded PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>")

# Copy SDL3 runtime next to the executables when built as a shared lib
if(WIN32)
    add_custom_command(TARGET q2ded POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:q2ded> $<TARGET_FILE_DIR:q2ded>
        COMMAND_EXPAND_LISTS)
endif()

# ---------------------------------------------------------------------------
# Game modules — dynamic libs with the original GetGameAPI ABI
# ---------------------------------------------------------------------------
function(q2_game_module NAME SRCDIR OUTDIR)
    file(GLOB MODULE_SOURCES ${SRCDIR}/*.c)
    add_library(${NAME} MODULE ${MODULE_SOURCES})
    target_include_directories(${NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${NAME} PRIVATE q2_common_flags)
    set_target_properties(${NAME} PROPERTIES
        OUTPUT_NAME "game_x86_64"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/${OUTDIR}>"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/${OUTDIR}>")
    if(MSVC)
        target_sources(${NAME} PRIVATE ${SRCDIR}/${NAME}.def)
    endif()
endfunction()

q2_game_module(game ${CMAKE_CURRENT_SOURCE_DIR}/src/game baseq2)
q2_game_module(ctf  ${CMAKE_CURRENT_SOURCE_DIR}/src/ctf  ctf)
