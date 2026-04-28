cmake_minimum_required(VERSION 3.16.5)
project(barton-rbus-provider C)

find_package(PkgConfig REQUIRED)

# ---- rbus -------------------------------------------------------------
find_library(RBUS_LIB     NAMES rbus     HINTS /usr/lib /usr/local/lib)
find_library(RBUSCORE_LIB NAMES rbuscore HINTS /usr/lib /usr/local/lib)
find_path(RBUS_INCLUDE_DIR NAMES rbus/rbus.h HINTS /usr/include /usr/local/include)

if(NOT RBUS_LIB)
    message(FATAL_ERROR "librbus.so not found")
endif()
if(NOT RBUSCORE_LIB)
    message(FATAL_ERROR "librbuscore.so not found")
endif()
message(STATUS "rbus lib    : ${RBUS_LIB}")
message(STATUS "rbuscore lib: ${RBUSCORE_LIB}")
message(STATUS "rbus include: ${RBUS_INCLUDE_DIR}")

# ---- rtmessage (needed by rbuscore.h) ---------------------------------
find_path(RTMESSAGE_INCLUDE_DIR NAMES rtMessageHeader.h
          HINTS /usr/include/rtmessage /usr/local/include/rtmessage)
if(NOT RTMESSAGE_INCLUDE_DIR)
    message(FATAL_ERROR "rtMessageHeader.h not found")
endif()
message(STATUS "rtmessage include: ${RTMESSAGE_INCLUDE_DIR}")

# ---- BartonCore -------------------------------------------------------
find_library(BARTONCORE_LIB NAMES BartonCore HINTS /usr/lib /usr/local/lib)
find_path(BARTONCORE_INCLUDE_DIR NAMES barton-core-client.h
          PATH_SUFFIXES barton HINTS /usr/include /usr/local/include)
if(NOT BARTONCORE_LIB)
    message(FATAL_ERROR "libBartonCore.so not found")
endif()
if(NOT BARTONCORE_INCLUDE_DIR)
    message(FATAL_ERROR "barton headers not found")
endif()
message(STATUS "BartonCore lib    : ${BARTONCORE_LIB}")
message(STATUS "BartonCore include: ${BARTONCORE_INCLUDE_DIR}")

# ---- GLib + GObject ---------------------------------------------------
pkg_check_modules(GLIB    REQUIRED glib-2.0)
pkg_check_modules(GOBJECT REQUIRED gobject-2.0)

# ---- Sources ----------------------------------------------------------
add_executable(barton-rbus-provider
    src/barton_rbus_provider.c
    src/rbus-network-credentials-provider.c
    src/main.c
)

target_include_directories(barton-rbus-provider PRIVATE
    src
    ${BARTONCORE_INCLUDE_DIR}
    ${RBUS_INCLUDE_DIR}
    ${RTMESSAGE_INCLUDE_DIR}        # <-- fixes rtMessageHeader.h
    ${GLIB_INCLUDE_DIRS}
    ${GOBJECT_INCLUDE_DIRS}
)

target_link_libraries(barton-rbus-provider PRIVATE
    ${RBUS_LIB}
    ${RBUSCORE_LIB}
    ${BARTONCORE_LIB}
    ${GOBJECT_LIBRARIES}
    ${GLIB_LIBRARIES}
    pthread
)

target_compile_options(barton-rbus-provider PRIVATE
    -Wall -Wextra
    ${GLIB_CFLAGS_OTHER}
    ${GOBJECT_CFLAGS_OTHER}
)

install(TARGETS barton-rbus-provider RUNTIME DESTINATION bin)
