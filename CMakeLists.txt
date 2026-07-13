cmake_minimum_required(VERSION 3.24)
project(quake2 LANGUAGES C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This port is 64-bit only. Configure a 64-bit toolchain.")
endif()

# C dialect is applied per-target via q2_common_flags (C23 / latest);
# a global CMAKE_C_STANDARD would leak into fetched deps like SDL3.

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
include(CheckCCompilerFlag)

add_library(q2_common_flags INTERFACE)
if(MSVC)
    target_compile_definitions(q2_common_flags INTERFACE
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE C_ONLY)
    target_compile_options(q2_common_flags INTERFACE /std:clatest /W3
        /guard:cf /sdl)          # control-flow guard + extra runtime checks
    target_link_options(q2_common_flags INTERFACE /guard:cf)
else()
    check_c_compiler_flag(-std=gnu23 HAVE_STD_GNU23)
    if(HAVE_STD_GNU23)
        target_compile_options(q2_common_flags INTERFACE -std=gnu23)
    else()
        target_compile_options(q2_common_flags INTERFACE -std=gnu2x)
    endif()
    target_compile_definitions(q2_common_flags INTERFACE C_ONLY)
    target_compile_options(q2_common_flags INTERFACE
        -fno-strict-aliasing -fwrapv
        -fstack-protector-strong
        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
        -Werror=format-security
        -Wall -Wno-unused-parameter -Wno-sign-compare
        -Wno-unused-but-set-variable)
endif()

# ---------------------------------------------------------------------------
# q2ded — dedicated server
# ---------------------------------------------------------------------------
set(QCOMMON_SOURCES
    src/common/cmd.c
    src/common/cmodel.c
    src/common/common.c
    src/common/crc.c
    src/common/cvar.c
    src/common/files.c
    src/common/md4.c
    src/common/net_chan.c
    src/common/pmove.c
    src/game/q_shared.c
)

set(SERVER_SOURCES
    src/server/ccmds.c
    src/server/ents.c
    src/server/game.c
    src/server/init.c
    src/server/main.c
    src/server/send.c
    src/server/user.c
    src/server/world.c
)

add_executable(q2ded
    ${QCOMMON_SOURCES}
    ${SERVER_SOURCES}
    src/platform/cl_stub.c
    src/platform/cd_stub.c
    src/platform/sys_sdl.c
    src/platform/net_sockets.c
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
