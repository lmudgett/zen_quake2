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
# Authenticode signing (Windows). Opt-in: configure with either
#   -DQ2_SIGN_SHA1=<cert thumbprint in the user's cert store>   (dev / CI)
# or
#   -DQ2_SIGN_PFX=<path.pfx> [-DQ2_SIGN_PFX_PASSWORD=<pw>]        (release)
# Signing every build keeps Windows from re-flagging the binary each run.
# ---------------------------------------------------------------------------
if(WIN32 AND (Q2_SIGN_SHA1 OR Q2_SIGN_PFX))
    find_program(Q2_SIGNTOOL signtool HINTS "$ENV{WindowsSdkVerBinPath}/x64")
    if(NOT Q2_SIGNTOOL)
        # signtool lives under bin/<sdk-version>/x64 — glob newest-first
        file(GLOB _q2_sdk_signtools
            "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/signtool.exe")
        if(_q2_sdk_signtools)
            list(SORT _q2_sdk_signtools ORDER DESCENDING)
            list(GET _q2_sdk_signtools 0 Q2_SIGNTOOL)
        endif()
    endif()
    if(NOT Q2_SIGNTOOL)
        message(FATAL_ERROR "Signing requested but signtool.exe was not found (install the Windows SDK).")
    endif()

    set(Q2_SIGN_TS "http://timestamp.digicert.com" CACHE STRING "RFC3161 timestamp URL")
    if(Q2_SIGN_PFX)
        set(_q2_sign_id /f "${Q2_SIGN_PFX}")
        if(Q2_SIGN_PFX_PASSWORD)
            list(APPEND _q2_sign_id /p "${Q2_SIGN_PFX_PASSWORD}")
        endif()
    else()
        set(_q2_sign_id /sha1 "${Q2_SIGN_SHA1}")
    endif()

    # Sign every built artifact (exe + game DLLs) after it links.
    function(q2_sign TGT)
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND "${Q2_SIGNTOOL}" sign /fd SHA256 ${_q2_sign_id}
                    /tr "${Q2_SIGN_TS}" /td SHA256 $<TARGET_FILE:${TGT}>
            VERBATIM
            COMMENT "Authenticode-signing ${TGT}")
    endfunction()
    q2_sign(q2ded)
    set(Q2_SIGN_ENABLED ON)
endif()

# ---------------------------------------------------------------------------
# quake2 — full client (integrated server). The renderer loads dynamically.
# ---------------------------------------------------------------------------
file(GLOB CLIENT_SOURCES src/client/*.c)
list(FILTER CLIENT_SOURCES EXCLUDE REGEX "/x86\\.c$")   # id386 RDTSC asm; unused on x64

add_executable(quake2
    ${QCOMMON_SOURCES}
    ${SERVER_SOURCES}
    ${CLIENT_SOURCES}
    src/game/m_flash.c              # muzzle-flash offset table, shared with the client
    src/platform/sys_sdl.c
    src/platform/net_sockets.c
    src/platform/vid_sdl.c
    src/platform/vid_menu.c
    src/platform/in_sdl.c
    src/platform/snd_sdl.c
    src/platform/cd_stub.c
)
target_include_directories(quake2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quake2 PRIVATE q2_common_flags SDL3::SDL3)
if(WIN32)
    target_link_libraries(quake2 PRIVATE ws2_32)
endif()
set_target_properties(quake2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>")
if(WIN32)
    add_custom_command(TARGET quake2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:quake2> $<TARGET_FILE_DIR:quake2>
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
    if(Q2_SIGN_ENABLED)
        q2_sign(${NAME})
    endif()
endfunction()

q2_game_module(game ${CMAKE_CURRENT_SOURCE_DIR}/src/game baseq2)
q2_game_module(ctf  ${CMAKE_CURRENT_SOURCE_DIR}/src/ctf  ctf)

# ---------------------------------------------------------------------------
# ref_gl3 — modern OpenGL 3.3 core renderer (loaded dynamically via GetRefAPI)
# ---------------------------------------------------------------------------
file(GLOB REF_GL3_SOURCES src/ref_gl3/gl3_*.c)
add_library(ref_gl3 MODULE
    ${REF_GL3_SOURCES}
    src/ref_gl3/glad/src/gl.c
    src/game/q_shared.c             # Com_sprintf/va/byte-swap utilities
)
target_include_directories(ref_gl3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ref_gl3/glad/include)
target_link_libraries(ref_gl3 PRIVATE q2_common_flags SDL3::SDL3)
set_target_properties(ref_gl3 PROPERTIES
    PREFIX ""            # produce ref_gl3.dll / ref_gl3.so, not libref_gl3
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>")
