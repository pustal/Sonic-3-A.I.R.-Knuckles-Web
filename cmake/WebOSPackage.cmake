# WebOSPackage.cmake
#
# Adds the "webos-package" target: stages the IPK layout under <build>/ipk/
#   appinfo.json, icon.png, largeIcon.png
#   Sonic3AIR                  the executable (A.I.R. and the webOS layer)
#   lib/libSDL2-2.0.so.0       SDL-webOS
#   game/                      the game data (cmake/StageGameData.cmake)
#   LICENSE, THIRD_PARTY_NOTICES.md, licenses/
# strips the binaries and builds dist/<id>_<version>_arm.ipk with ares-package
# (from @webos-tools/cli), plus dist/<id>.manifest.json for the webOS Homebrew
# Channel. Also adds "webos-install" (ares-install to S3W_ARES_DEVICE, or the
# default device).

find_program(S3W_ARES_PACKAGE ares-package)
find_program(S3W_ARES_INSTALL ares-install)

set(S3W_IPK_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ipk")
set(S3W_DIST_DIR "${S3W_ROOT}/dist")
set(S3W_IPK_NAME "${S3W_APP_ID}_${PROJECT_VERSION}_arm.ipk")
set(S3W_MANIFEST_NAME "${S3W_APP_ID}.manifest.json")
set(S3W_PROJECT_URL "https://github.com/pustal/Sonic-3-A.I.R.-Knuckles-Web")
set(S3W_ICON_URI "https://raw.githubusercontent.com/pustal/Sonic-3-A.I.R.-Knuckles-Web/main/webos/largeIcon.png")
set(S3W_AIR_LICENSES "${AIR_GAME_DIR}/_master_image_template/doc")

configure_file("${S3W_ROOT}/webos/appinfo.json.in" "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" @ONLY)

if(NOT CMAKE_STRIP)
    set(CMAKE_STRIP strip)
endif()

add_custom_target(webos-stage
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${S3W_IPK_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${S3W_IPK_STAGE}/lib" "${S3W_IPK_STAGE}/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" "${S3W_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${S3W_ROOT}/webos/icon.png" "${S3W_ROOT}/webos/largeIcon.png" "${S3W_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:Sonic3AIR>" "${S3W_IPK_STAGE}/Sonic3AIR"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${S3W_IPK_STAGE}/Sonic3AIR"
    COMMAND "${CMAKE_COMMAND}" -E copy "${S3W_SDL2_RUNTIME_LIBRARY}" "${S3W_IPK_STAGE}/lib/"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${S3W_IPK_STAGE}/lib/libSDL2-2.0.so.0"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${S3W_GAME_DATA_DIR}" "${S3W_IPK_STAGE}/game"
    COMMAND "${CMAKE_COMMAND}" -E copy "${S3W_ROOT}/LICENSE" "${S3W_ROOT}/THIRD_PARTY_NOTICES.md" "${S3W_IPK_STAGE}/"
    # The licences' full texts, which their terms ask to ship with the binaries:
    # this port's (licenses/), and those of the libraries A.I.R. builds in.
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${S3W_ROOT}/licenses" "${S3W_IPK_STAGE}/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy "${S3W_AIR_LICENSES}/license.txt" "${S3W_IPK_STAGE}/licenses/Sonic3AIR-license.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${S3W_AIR_LICENSES}/license-thirdparty/jsoncpp_LICENSE.txt"
            "${S3W_AIR_LICENSES}/license-thirdparty/libogg_COPYING.txt"
            "${S3W_AIR_LICENSES}/license-thirdparty/libvorbis_COPYING.txt"
            "${S3W_AIR_LICENSES}/license-thirdparty/zlib_README.txt"
            "${S3W_IPK_STAGE}/licenses/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${S3W_AIR_LICENSES}/license-thirdparty/freefont/COPYING" "${S3W_IPK_STAGE}/licenses/freefont_COPYING.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${S3W_AIR_LICENSES}/license-thirdparty/shader" "${S3W_IPK_STAGE}/licenses/shader"
    DEPENDS Sonic3AIR game-data
    COMMENT "Staging the IPK contents in ${S3W_IPK_STAGE}"
    VERBATIM
)

if(S3W_ARES_PACKAGE)
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${S3W_DIST_DIR}"
        COMMAND "${S3W_ARES_PACKAGE}" --no-minify "${S3W_IPK_STAGE}" -o "${S3W_DIST_DIR}"
        COMMAND "${CMAKE_COMMAND}" -DSTAGE=${S3W_IPK_STAGE} -DIPK=${S3W_DIST_DIR}/${S3W_IPK_NAME}
                -P "${S3W_ROOT}/cmake/ReportFootprint.cmake"
        COMMAND "${CMAKE_COMMAND}"
                -DIPK=${S3W_DIST_DIR}/${S3W_IPK_NAME}
                -DOUTPUT=${S3W_DIST_DIR}/${S3W_MANIFEST_NAME}
                -DAPP_ID=${S3W_APP_ID}
                -DVERSION=${PROJECT_VERSION}
                "-DTITLE=Sonic 3 A.I.R."
                "-DDESCRIPTION=Sonic 3 - Angel Island Revisited for webOS. Bring your own Sonic 3 & Knuckles ROM."
                -DICON_URI=${S3W_ICON_URI}
                -DSOURCE_URL=${S3W_PROJECT_URL}
                -P "${S3W_ROOT}/cmake/WriteManifest.cmake"
        DEPENDS webos-stage
        COMMENT "Building ${S3W_IPK_NAME}"
        VERBATIM
    )
else()
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E echo "ares-package not found: npm install -g @webos-tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E false
        DEPENDS webos-stage
    )
endif()

set(S3W_ARES_DEVICE "" CACHE STRING "ares device name for webos-install (empty = ares default device)")
if(S3W_ARES_INSTALL)
    set(_s3w_device_args)
    if(S3W_ARES_DEVICE)
        set(_s3w_device_args -d "${S3W_ARES_DEVICE}")
    endif()
    add_custom_target(webos-install
        COMMAND "${S3W_ARES_INSTALL}" ${_s3w_device_args} "${S3W_DIST_DIR}/${S3W_IPK_NAME}"
        DEPENDS webos-package
        COMMENT "Installing ${S3W_IPK_NAME} on the TV"
        VERBATIM
    )
endif()
