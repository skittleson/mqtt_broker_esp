cmake_minimum_required(VERSION 3.16)

# FD_SETSIZE must be >= CONFIG_LWIP_MAX_SOCKETS + 6 (for VFS console FDs).
# With 115 max sockets, we need at least 121. Use 128 for headroom.
add_compile_definitions(FD_SETSIZE=128)

# Mirror FW_VERSION from main/version.h into IDF's PROJECT_VER so that the
# `esp_app_desc_t.version` field embedded in the OTA image header matches
# what the portal displays. Without this, the field defaulted to
# `git describe` output (e.g. `tester-v0.3.0-4-g30c64db-dirty`) which made
# the /update page's "Other Partition" row — and therefore the Rollback
# panel — misleading.
#
# Parse FW_VERSION out of main/version.h at configure time so there's a
# single source of truth.
file(READ "${CMAKE_SOURCE_DIR}/main/version.h" _version_h)
if(_version_h MATCHES "#define[ \t]+FW_VERSION[ \t]+\"([^\"]+)\"")
    set(PROJECT_VER "${CMAKE_MATCH_1}")
    message(STATUS "mqtt_broker: PROJECT_VER=${PROJECT_VER} (from main/version.h)")
endif()
# Re-run configure automatically whenever version.h changes, so bumping
# FW_VERSION doesn't silently re-use the cached PROJECT_VER from a previous
# build (which would embed the wrong version in esp_app_desc_t).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/main/version.h")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mqtt_broker)
