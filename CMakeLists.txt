# Sonic 3 A.I.R. for LG webOS TVs.
#
# Builds Eukaryot's Sonic 3 - Angel Island Revisited (external/sonic3air, the
# Oxygen engine and its libraries) natively for the TV, with SDL-webOS and
# OpenGL ES 2, and wraps it with the webOS layer in src/: the transfer page
# (ROM, saves, mods, the remastered soundtrack), USB drive support, the setup
# screen, the in-game menu and the remote control mapping.
#
#   cmake -B build-webos -G Ninja -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake
#   cmake --build build-webos && cmake --build build-webos --target webos-package
#
# Without the webOS toolchain the same sources build as a desktop Linux program
# (system SDL2 and OpenGL ES 2), which is how the transfer page, the setup
# screen and the menu are tested without a TV.

cmake_minimum_required(VERSION 3.21)
project(Sonic3AIRWebOS VERSION 1.0.1 LANGUAGES C CXX)

set(S3W_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${S3W_ROOT}/cmake")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(S3W_APP_ID "org.sonic3air.webos" CACHE STRING "webOS application id")

# ---------------------------------------------------------------------------
# Target: the TV (webOS toolchain) or a desktop Linux test build
# ---------------------------------------------------------------------------
execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                OUTPUT_VARIABLE S3W_COMPILER_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(S3W_COMPILER_TARGET MATCHES "webos")
    set(S3W_TARGET_WEBOS ON)
else()
    set(S3W_TARGET_WEBOS OFF)
    message(STATUS "Not the webOS toolchain (${S3W_COMPILER_TARGET}): desktop test build")
endif()

set(AIR_ROOT "${S3W_ROOT}/external/sonic3air")
if(NOT EXISTS "${AIR_ROOT}/Oxygen/sonic3air/source/sonic3air/main.cpp")
    message(FATAL_ERROR "Missing ${AIR_ROOT}. Run: git submodule update --init")
endif()
set(AIR_GAME_DIR "${AIR_ROOT}/Oxygen/sonic3air")
# librmx, lemonscript, the Oxygen engine and the game's code are built from a patched copy (AIR_SRC): see the file and patches/.
include(PatchUpstream)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

if(S3W_TARGET_WEBOS)
    include(SDL2WebOSBackport)   # SDL2::SDL2 (bundled SDL-webOS 2.30)
    # The sysroot's GLES 2 library; the TV's own driver is loaded at run time.
    find_library(S3W_GLESV2 GLESv2 REQUIRED)
else()
    find_package(SDL2 REQUIRED)
    if(NOT TARGET SDL2::SDL2)
        add_library(SDL2::SDL2 INTERFACE IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${SDL2_LIBRARIES}")
    endif()
    find_library(S3W_GLESV2 GLESv2 REQUIRED)
endif()

# rmxmedia includes <SDL2/SDL.h> on Linux, the port <SDL.h>: both must find the
# same SDL (the bundled SDL-webOS on the TV, not the sysroot's older one).
get_target_property(_s3w_sdl_include SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
list(GET _s3w_sdl_include 0 _s3w_sdl_include)
get_filename_component(_s3w_sdl_include_parent "${_s3w_sdl_include}" DIRECTORY)

# ---------------------------------------------------------------------------
# The game: Oxygen engine, lemonscript, librmx and their externals, as the
# upstream Linux build (Oxygen/sonic3air/build/_cmake) compiles them with its
# USE_GLES option, without ImGui (dev mode windows) and Discord.
# ---------------------------------------------------------------------------
add_compile_definitions(RMX_LINUX_ENFORCE_GLES2)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # The same warnings the upstream build silences.
    add_compile_options(-Wno-unused-function -Wno-stringop-overflow -Wno-psabi)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Unwind tables everywhere, so the crash handler (src/port/Log.cpp) can list
# the calls that led to a crash on the TV's 32-bit ARM too.
add_compile_options(-funwind-tables)
# The game's own code is upstream's; its warnings are not ours to fix.
set(AIR_COMPILE_OPTIONS -w)

set(AIR_INCLUDE_DIRS
    "${_s3w_sdl_include_parent}"
    "${_s3w_sdl_include}"
    "${AIR_ROOT}/framework/external/ogg-vorbis/libogg/include"
    "${AIR_ROOT}/framework/external/ogg-vorbis/libvorbis/include"
    "${AIR_ROOT}/framework/external/ogg-vorbis/libvorbis/lib"
    "${AIR_ROOT}/framework/external/zlib/zlib/contrib/minizip"
    # The curl API header only: libcurl itself is loaded at run time (src/port/CurlShim.cpp).
    "${AIR_ROOT}/framework/external/curl/curl/include"
    "${AIR_SRC}/librmx/source"
    "${AIR_SRC}/Oxygen/lemonscript/source"
    "${AIR_SRC}/Oxygen/oxygenengine/source"
    "${AIR_SRC}/Oxygen/sonic3air/source"
    "${AIR_GAME_DIR}/source/external")

# Each library below gets these include directories and warning settings.
function(s3w_air_library name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC ${AIR_INCLUDE_DIRS})
    target_compile_options(${name} PRIVATE ${AIR_COMPILE_OPTIONS})
endfunction()

# ogg vorbis
file(GLOB AIR_OGGVORBIS_SOURCES
    "${AIR_ROOT}/framework/external/ogg-vorbis/libogg/src/*.c"
    "${AIR_ROOT}/framework/external/ogg-vorbis/libvorbis/lib/*.c")
s3w_air_library(air_oggvorbis ${AIR_OGGVORBIS_SOURCES})

# minizip (zip mods, and the save backups of the transfer page)
s3w_air_library(air_minizip
    "${AIR_ROOT}/framework/external/zlib/zlib/contrib/minizip/ioapi.c"
    "${AIR_ROOT}/framework/external/zlib/zlib/contrib/minizip/unzip.c"
    "${AIR_ROOT}/framework/external/zlib/zlib/contrib/minizip/zip.c")
target_link_libraries(air_minizip PUBLIC ZLIB::ZLIB)
# minizip calls fopen64/fseeko64/ftello64, which glibc only declares with
# _LARGEFILE64_SOURCE; undeclared, C assumes they return int and the FILE
# pointer is cut to 32 bits (a crash on 64-bit desktops).
target_compile_definitions(air_minizip PRIVATE _LARGEFILE64_SOURCE)

# rmxbase
file(GLOB_RECURSE AIR_RMXBASE_SOURCES "${AIR_SRC}/librmx/source/rmxbase/*.cpp")
s3w_air_library(air_rmxbase ${AIR_RMXBASE_SOURCES})
target_precompile_headers(air_rmxbase PRIVATE "${AIR_SRC}/librmx/source/rmxbase.h")
target_link_libraries(air_rmxbase PUBLIC ZLIB::ZLIB)

# rmxmedia (GLEW is not used with OpenGL ES, and needs X11: left out)
file(GLOB_RECURSE AIR_RMXMEDIA_SOURCES "${AIR_SRC}/librmx/source/rmxmedia/*.cpp")
s3w_air_library(air_rmxmedia ${AIR_RMXMEDIA_SOURCES})
target_link_libraries(air_rmxmedia PUBLIC air_rmxbase SDL2::SDL2 "${S3W_GLESV2}")

# rmxext_oggvorbis
file(GLOB AIR_RMXEXT_OGGVORBIS_SOURCES "${AIR_SRC}/librmx/source/rmxext_oggvorbis/*.cpp")
s3w_air_library(air_rmxext_oggvorbis ${AIR_RMXEXT_OGGVORBIS_SOURCES})
target_link_libraries(air_rmxext_oggvorbis PUBLIC air_rmxmedia air_oggvorbis)

# lemonscript
file(GLOB_RECURSE AIR_LEMONSCRIPT_SOURCES "${AIR_SRC}/Oxygen/lemonscript/source/lemon/*.cpp")
s3w_air_library(air_lemonscript ${AIR_LEMONSCRIPT_SOURCES})
target_precompile_headers(air_lemonscript PRIVATE "${AIR_SRC}/Oxygen/lemonscript/source/lemon/pch.h")
target_link_libraries(air_lemonscript PUBLIC air_rmxbase)

# oxygen_netcore
file(GLOB_RECURSE AIR_NETCORE_SOURCES "${AIR_SRC}/Oxygen/oxygenengine/source/oxygen_netcore/*.cpp")
s3w_air_library(air_netcore ${AIR_NETCORE_SOURCES})
target_precompile_headers(air_netcore PRIVATE "${AIR_SRC}/Oxygen/oxygenengine/source/oxygen_netcore/pch.h")
target_link_libraries(air_netcore PUBLIC air_rmxbase)

# oxygen
file(GLOB_RECURSE AIR_OXYGEN_SOURCES "${AIR_SRC}/Oxygen/oxygenengine/source/oxygen/*.cpp")
s3w_air_library(air_oxygen ${AIR_OXYGEN_SOURCES})
# GL_MIN/GL_MAX for the OpenGL drawer's blend modes (see the header).
target_compile_options(air_oxygen PRIVATE "SHELL:-include \"${S3W_ROOT}/src/port/GlesCompat.h\"")
target_link_libraries(air_oxygen PUBLIC air_minizip air_rmxmedia air_rmxext_oggvorbis air_lemonscript air_netcore
                                        Threads::Threads ${CMAKE_DL_LIBS})

# Sonic 3 A.I.R. itself. Its main() becomes s3air_main(), which src/main.cpp
# calls once the ROM is in place.
file(GLOB_RECURSE AIR_GAME_SOURCES "${AIR_SRC}/Oxygen/sonic3air/source/sonic3air/*.cpp")
s3w_air_library(air_game ${AIR_GAME_SOURCES})
target_precompile_headers(air_game PRIVATE "${AIR_SRC}/Oxygen/sonic3air/source/sonic3air/pch.h")
set_source_files_properties("${AIR_SRC}/Oxygen/sonic3air/source/sonic3air/main.cpp" PROPERTIES
    COMPILE_DEFINITIONS "main=s3air_main"
    SKIP_PRECOMPILE_HEADERS ON)
target_link_libraries(air_game PUBLIC air_oxygen)
# "Check for updates" asks for this port's GitHub releases (patches/0003,
# src/port/PortUpdateCheck.cpp) instead of Eukaryot's official builds.
target_compile_definitions(air_game PRIVATE S3W_PORT_UPDATE_CHECK)

# The game's version, for the transfer page and the menu.
file(STRINGS "${AIR_SRC}/Oxygen/sonic3air/source/sonic3air/version.inc" _s3w_build_string REGEX "BUILD_STRING")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" S3W_AIR_VERSION "${_s3w_build_string}")
message(STATUS "Sonic 3 A.I.R. ${S3W_AIR_VERSION}")

# ---------------------------------------------------------------------------
# The webOS layer (src/)
# ---------------------------------------------------------------------------
set(S3W_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${S3W_GENERATED_DIR}/WebPage.generated.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${S3W_GENERATED_DIR}"
    COMMAND "${CMAKE_COMMAND}"
            -DINPUT=${S3W_ROOT}/web/index.html
            -DOUTPUT=${S3W_GENERATED_DIR}/WebPage.generated.h
            -DVARIABLE=kWebPageHtml
            -P "${S3W_ROOT}/cmake/EmbedTextFile.cmake"
    DEPENDS "${S3W_ROOT}/web/index.html" "${S3W_ROOT}/cmake/EmbedTextFile.cmake"
    COMMENT "Embedding the transfer page")

# The game data the app ships, next to the executable (<build>/game): see
# cmake/StageGameData.cmake. A desktop build runs from the build folder as is.
set(S3W_GAME_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/game")
file(GLOB_RECURSE _s3w_game_data_inputs CONFIGURE_DEPENDS
    "${AIR_GAME_DIR}/data/*" "${AIR_GAME_DIR}/scripts/*" "${AIR_ROOT}/Oxygen/oxygenengine/data/*")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/game.stamp"
    COMMAND "${CMAKE_COMMAND}"
            -DAIR_GAME_DIR=${AIR_GAME_DIR}
            -DAIR_ENGINE_DATA=${AIR_ROOT}/Oxygen/oxygenengine/data
            -DCONFIG=${S3W_ROOT}/webos/config.json
            -DPROJECT=${S3W_ROOT}/webos/oxygenproject.json
            -DOUTPUT=${S3W_GAME_DATA_DIR}
            -DSTAMP=${CMAKE_CURRENT_BINARY_DIR}/game.stamp
            -P "${S3W_ROOT}/cmake/StageGameData.cmake"
    DEPENDS ${_s3w_game_data_inputs} "${S3W_ROOT}/webos/config.json" "${S3W_ROOT}/webos/oxygenproject.json"
            "${S3W_ROOT}/cmake/StageGameData.cmake"
    COMMENT "Staging the game data")
add_custom_target(game-data DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/game.stamp")

set(S3W_PORT_SOURCES
    src/main.cpp
    src/port/BitmapFont.cpp
    src/port/CurlShim.cpp
    src/port/GameFolder.cpp
    src/port/Log.cpp
    src/port/MenuOverlay.cpp
    src/port/NetInfo.cpp
    src/port/PortUpdateCheck.cpp
    src/port/RemoteInput.cpp
    src/port/SaveBackup.cpp
    src/port/SdlHooks.cpp
    src/port/SetupScreen.cpp
    src/port/Storage.cpp
    src/port/TextRenderer.cpp
    src/port/TransferServer.cpp
    src/port/UsbAccess.cpp)

add_executable(Sonic3AIR ${S3W_PORT_SOURCES} "${S3W_GENERATED_DIR}/WebPage.generated.h")
add_dependencies(Sonic3AIR game-data)
target_compile_options(Sonic3AIR PRIVATE -Wall -Wextra)
# 64-bit file offsets and sizes on the TV's 32-bit system (large USB drives, uploads).
target_compile_definitions(Sonic3AIR PRIVATE _FILE_OFFSET_BITS=64)
target_compile_definitions(Sonic3AIR PRIVATE
    "S3W_VERSION=\"${PROJECT_VERSION}\""
    "S3W_AIR_VERSION=\"${S3W_AIR_VERSION}\""
    "S3W_PROJECT_URL=\"https://github.com/pustal/Sonic-3-A.I.R.-Knuckles-Web/\""
    $<$<BOOL:${S3W_TARGET_WEBOS}>:S3W_TARGET_WEBOS>)
target_include_directories(Sonic3AIR PRIVATE "${S3W_GENERATED_DIR}" "${S3W_ROOT}/src" "${S3W_ROOT}/src/port"
                                             "${_s3w_sdl_include_parent}" "${_s3w_sdl_include}")
# cpp-httplib (MIT, single header) as a system header: its warnings are not ours.
target_include_directories(Sonic3AIR SYSTEM PRIVATE "${S3W_ROOT}/third_party/cpp-httplib")
target_link_libraries(Sonic3AIR PRIVATE air_game SDL2::SDL2 "${S3W_GLESV2}" Threads::Threads ${CMAKE_DL_LIBS})

# src/port/SdlHooks.cpp sees the game's SDL calls first: the event queue (the
# remote and the menu keys), the buffer swap (the menu overlay) and the audio
# device (paused while the menu is open).
target_link_options(Sonic3AIR PRIVATE
    -Wl,--wrap=SDL_PollEvent
    -Wl,--wrap=SDL_GL_SwapWindow
    -Wl,--wrap=SDL_OpenAudioDevice
    -Wl,--wrap=SDL_CreateWindow
    -Wl,--wrap=SDL_ShowMessageBox
    -Wl,--wrap=SDL_ShowSimpleMessageBox)

if(S3W_TARGET_WEBOS)
    # The TV's C++ runtime is older than the toolchain's; carry our own.
    target_link_options(Sonic3AIR PRIVATE -static-libstdc++ -static-libgcc)
    set_target_properties(Sonic3AIR PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "\$ORIGIN/lib")
    include(WebOSPackage)
endif()
