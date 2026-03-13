cmake_minimum_required(VERSION 3.16)
project(PlotJugglerLogixTrend VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── Dependencies ─────────────────────────────────────────────────────────────

find_package(Qt5 REQUIRED COMPONENTS Core Widgets Xml)
find_package(plotjuggler QUIET)

# If plotjuggler package not found via CMake, allow manual specification
if(NOT plotjuggler_FOUND)
    message(STATUS "plotjuggler CMake package not found, using PLOTJUGGLER_SRC_DIR")
    if(NOT DEFINED PLOTJUGGLER_SRC_DIR)
        message(FATAL_ERROR
            "PlotJuggler not found. Set PLOTJUGGLER_SRC_DIR to the PlotJuggler source root "
            "(e.g. -DPLOTJUGGLER_SRC_DIR=/path/to/PlotJuggler)")
    endif()
    if(NOT DEFINED PLOTJUGGLER_BUILD_DIR)
        set(PLOTJUGGLER_BUILD_DIR "${PLOTJUGGLER_SRC_DIR}/build")
    endif()
endif()

# ── Plugin Target ────────────────────────────────────────────────────────────

set(PLUGIN_SOURCES
    src/logix_eip.cpp
    src/logix_tag_browser.cpp
    src/logix_trend.cpp
    src/logix_config_dialog.cpp
    src/datastream_logix_trend.cpp
)

set(PLUGIN_HEADERS
    src/logix_eip.h
    src/logix_tag_browser.h
    src/logix_trend.h
    src/logix_config_dialog.h
    src/datastream_logix_trend.h
)

add_library(DataStreamLogixTrend SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(DataStreamLogixTrend PRIVATE
    Qt5::Core
    Qt5::Widgets
    Qt5::Xml
)

if(WIN32)
    target_link_libraries(DataStreamLogixTrend PRIVATE ws2_32)
endif()

if(plotjuggler_FOUND)
    target_link_libraries(DataStreamLogixTrend PRIVATE plotjuggler::plotjuggler_base)
else()
    target_include_directories(DataStreamLogixTrend PRIVATE
        ${PLOTJUGGLER_SRC_DIR}/plotjuggler_base/include
        ${PLOTJUGGLER_SRC_DIR}/3rdparty/qwt/src
        ${PLOTJUGGLER_SRC_DIR}/3rdparty/sol2-3.5.0/include
    )
    target_link_libraries(DataStreamLogixTrend PRIVATE
        ${PLOTJUGGLER_BUILD_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}plotjuggler_base${CMAKE_STATIC_LIBRARY_SUFFIX}
        ${PLOTJUGGLER_BUILD_DIR}/3rdparty/qwt/src/${CMAKE_STATIC_LIBRARY_PREFIX}plotjuggler_qwt${CMAKE_STATIC_LIBRARY_SUFFIX}
        ${PLOTJUGGLER_BUILD_DIR}/_deps/fmt-build/${CMAKE_STATIC_LIBRARY_PREFIX}fmt${CMAKE_STATIC_LIBRARY_SUFFIX}
    )
    # PlotJuggler version defines needed by headers
    target_compile_definitions(DataStreamLogixTrend PRIVATE
        PJ_MAJOR_VERSION=3
        PJ_MINOR_VERSION=16
        PJ_PATCH_VERSION=0
        FMT_HEADER_ONLY
    )
endif()

# ── Install ──────────────────────────────────────────────────────────────────

# Default install to PlotJuggler plugin directory
if(NOT DEFINED PLUGIN_INSTALL_DIR)
    set(PLUGIN_INSTALL_DIR lib)
endif()

install(TARGETS DataStreamLogixTrend
    LIBRARY DESTINATION ${PLUGIN_INSTALL_DIR}
    RUNTIME DESTINATION ${PLUGIN_INSTALL_DIR}
)
