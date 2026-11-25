#
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
#
# SPDX-License-Identifier: MIT
# Packaging and install layout for CardputerZero Debian builds.

include(GNUInstallDirs)

set(APP_DISPLAY_NAME "ZeroCast" CACHE STRING "Human-readable application name used by launchers and package filename")
set(APP_DEBIAN_REVISION "rev1" CACHE STRING "Debian package revision/vendor suffix")
set(APP_DEBIAN_ARCHITECTURE "arm64" CACHE STRING "Debian package architecture")
# The store rejects packages whose Maintainer is still the template/M5Stack identity.
# A GitHub noreply address keeps a personal email out of the public package.
set(APP_MAINTAINER "Josef H.B. Schneider <git@netpage.dk>" CACHE STRING "Debian package maintainer" FORCE)
set(APP_PACKAGE_DESCRIPTION "Live-stream the CardputerZero camera, mic or screen to Twitch, YouTube and Restreamer" CACHE STRING "Debian package summary" FORCE)
# ZeroCast is an interactive screen app started from APPLaunch; it is not a background service.
set(APP_INSTALL_SYSTEMD_SERVICE OFF CACHE BOOL "Install a systemd service file for embedded deployments")

set(APP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/package")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/templates/app.desktop.in"
    "${APP_GENERATED_DIR}/${PROJECT_NAME}.desktop"
    @ONLY
)

if(APP_INSTALL_SYSTEMD_SERVICE)
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/templates/app.service.in"
        "${APP_GENERATED_DIR}/${PROJECT_NAME}.service"
        @ONLY
    )
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/zerocast.conf"
    DESTINATION "/etc"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${APP_NAME}/audio"
    PATTERN ".DS_Store" EXCLUDE
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${APP_NAME}/fonts"
    PATTERN ".DS_Store" EXCLUDE
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${APP_NAME}/images"
    PATTERN ".DS_Store" EXCLUDE
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/APPLaunch/share/images"
    FILES_MATCHING
    PATTERN "zerocast*.png"
)

install(FILES "${APP_GENERATED_DIR}/${PROJECT_NAME}.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/APPLaunch/applications"
)

if(APP_INSTALL_SYSTEMD_SERVICE)
    install(FILES "${APP_GENERATED_DIR}/${PROJECT_NAME}.service"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
    )
endif()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME "third-party-assets-license.txt"
)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_PACKAGE_NAME "${APP_DISPLAY_NAME}")
set(CPACK_PACKAGE_VENDOR "${APP_MAINTAINER}")
set(CPACK_PACKAGE_CONTACT "${APP_MAINTAINER}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${APP_PACKAGE_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${APP_DISPLAY_NAME}_${PROJECT_VERSION}_${APP_DEBIAN_REVISION}_${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LICENSE.txt")

string(TOLOWER "${APP_DISPLAY_NAME}" APP_DEBIAN_PACKAGE_NAME)
string(REGEX REPLACE "[^a-z0-9+.-]" "-" APP_DEBIAN_PACKAGE_NAME "${APP_DEBIAN_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_NAME "${APP_DEBIAN_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "${APP_DEBIAN_REVISION}")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${APP_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# ffmpeg muxes and sends the stream, rpicam-apps captures the camera, pactl (pulseaudio-utils) builds the
# temporary mic + system-audio mix sink. wf-recorder is only needed for the optional HDMI/desktop capture,
# v4l-utils (v4l2-ctl) lets ZeroCast read what a USB camera offers (without it ffmpeg is asked instead).
# pactl and ffmpeg's pulse input talk to a sound server: pipewire-pulse (Pi OS default) or pulseaudio.
set(APP_DEBIAN_DEPENDS "libc6, libstdc++6, libgcc-s1, libfreetype6, libpng16-16t64 | libpng16-16, libjpeg62-turbo, zlib1g, ffmpeg, rpicam-apps-core | rpicam-apps-lite | rpicam-apps, pulseaudio-utils")
if(FMT_INCLUDE_DIR AND FMT_LIBRARY)
    string(APPEND APP_DEBIAN_DEPENDS ", libfmt10")
endif()
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${APP_DEBIAN_DEPENDS}")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "wf-recorder, v4l-utils, pipewire-pulse | pulseaudio")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/templates/conffiles.in"
    "${APP_GENERATED_DIR}/conffiles"
    @ONLY
)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${APP_GENERATED_DIR}/conffiles")

include(CPack)
