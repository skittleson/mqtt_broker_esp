cmake_minimum_required(VERSION 3.16)

# FD_SETSIZE must be >= CONFIG_LWIP_MAX_SOCKETS + 6 (for VFS console FDs).
# With 115 max sockets, we need at least 121. Use 128 for headroom.
add_compile_definitions(FD_SETSIZE=128)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mqtt_broker)
