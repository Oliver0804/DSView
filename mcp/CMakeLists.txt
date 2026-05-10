cmake_minimum_required(VERSION 3.13)
project(dsview_mcp_helper C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(DSVIEW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

include(FindPkgConfig)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
find_package(ZLIB REQUIRED)
find_package(Python3 COMPONENTS Development REQUIRED)

# libsigrok4DSL sources include <libusb-1.0/libusb.h>, so we need the
# parent of the libusb pkg-config include dir.
set(LIBUSB_PARENT_INCLUDE_DIRS "")
foreach(d IN LISTS LIBUSB_INCLUDE_DIRS)
    get_filename_component(_p "${d}" DIRECTORY)
    list(APPEND LIBUSB_PARENT_INCLUDE_DIRS "${_p}")
endforeach()

list(APPEND CMAKE_MODULE_PATH "${DSVIEW_ROOT}/CMake")

set(LIBSIGROK4DSL_SOURCES
    ${DSVIEW_ROOT}/libsigrok4DSL/version.c
    ${DSVIEW_ROOT}/libsigrok4DSL/strutil.c
    ${DSVIEW_ROOT}/libsigrok4DSL/std.c
    ${DSVIEW_ROOT}/libsigrok4DSL/session_driver.c
    ${DSVIEW_ROOT}/libsigrok4DSL/session.c
    ${DSVIEW_ROOT}/libsigrok4DSL/log.c
    ${DSVIEW_ROOT}/libsigrok4DSL/hwdriver.c
    ${DSVIEW_ROOT}/libsigrok4DSL/error.c
    ${DSVIEW_ROOT}/libsigrok4DSL/backend.c
    ${DSVIEW_ROOT}/libsigrok4DSL/output/output.c
    ${DSVIEW_ROOT}/libsigrok4DSL/input/input.c
    ${DSVIEW_ROOT}/libsigrok4DSL/hardware/demo/demo.c
    ${DSVIEW_ROOT}/libsigrok4DSL/input/in_binary.c
    ${DSVIEW_ROOT}/libsigrok4DSL/input/in_vcd.c
    ${DSVIEW_ROOT}/libsigrok4DSL/input/in_wav.c
    ${DSVIEW_ROOT}/libsigrok4DSL/output/csv.c
    ${DSVIEW_ROOT}/libsigrok4DSL/output/gnuplot.c
    ${DSVIEW_ROOT}/libsigrok4DSL/output/srzip.c
    ${DSVIEW_ROOT}/libsigrok4DSL/output/vcd.c
    ${DSVIEW_ROOT}/libsigrok4DSL/hardware/DSL/dslogic.c
    ${DSVIEW_ROOT}/libsigrok4DSL/hardware/DSL/dscope.c
    ${DSVIEW_ROOT}/libsigrok4DSL/hardware/DSL/command.c
    ${DSVIEW_ROOT}/libsigrok4DSL/hardware/DSL/dsl.c
    ${DSVIEW_ROOT}/libsigrok4DSL/hardware/common/usb.c
    ${DSVIEW_ROOT}/libsigrok4DSL/hardware/common/ezusb.c
    ${DSVIEW_ROOT}/libsigrok4DSL/trigger.c
    ${DSVIEW_ROOT}/libsigrok4DSL/dsdevice.c
    ${DSVIEW_ROOT}/libsigrok4DSL/lib_main.c
)

set(COMMON_SOURCES
    ${DSVIEW_ROOT}/common/minizip/zip.c
    ${DSVIEW_ROOT}/common/minizip/unzip.c
    ${DSVIEW_ROOT}/common/minizip/ioapi.c
    ${DSVIEW_ROOT}/common/log/xlog.c
)

set(LIBSIGROKDECODE4DSL_SOURCES
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/type_decoder.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/srd.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/module_sigrokdecode.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/decoder.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/error.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/exception.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/instance.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/log.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/session.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/util.c
    ${DSVIEW_ROOT}/libsigrokdecode4DSL/version.c
)

add_executable(dsview_helper
    dsview_helper/main.c
    dsview_helper/decode.c
    ${LIBSIGROK4DSL_SOURCES}
    ${LIBSIGROKDECODE4DSL_SOURCES}
    ${COMMON_SOURCES}
)

target_include_directories(dsview_helper PRIVATE
    ${DSVIEW_ROOT}
    ${DSVIEW_ROOT}/libsigrok4DSL
    ${DSVIEW_ROOT}/libsigrokdecode4DSL
    ${DSVIEW_ROOT}/common
    ${GLIB_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIRS}
    ${LIBUSB_PARENT_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${Python3_INCLUDE_DIRS}
)

target_link_directories(dsview_helper PRIVATE
    ${GLIB_LIBRARY_DIRS}
    ${LIBUSB_LIBRARY_DIRS}
)

target_link_libraries(dsview_helper PRIVATE
    ${GLIB_LIBRARIES}
    ${LIBUSB_LIBRARIES}
    ${ZLIB_LIBRARIES}
    Python3::Python
)

target_compile_options(dsview_helper PRIVATE
    -Wno-return-type
    -Wno-ignored-qualifiers
    -Wno-deprecated-declarations
    -Wno-pointer-sign
    -Wno-unused-result
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
)

if(APPLE)
    target_link_libraries(dsview_helper PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit"
    )
endif()

install(TARGETS dsview_helper RUNTIME DESTINATION bin)
