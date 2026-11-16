# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
cmake_minimum_required(VERSION 3.21)
project(LauncherDocumentation LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Test)
set(LAUNCHER "${CMAKE_CURRENT_SOURCE_DIR}/../../launcher")
add_subdirectory("${LAUNCHER}" launcher EXCLUDE_FROM_ALL)
qt_add_executable(launcher-capture capture.cpp
    "${LAUNCHER}/window.cpp" "${LAUNCHER}/window.h"
    "${LAUNCHER}/artwork.cpp" "${LAUNCHER}/theme.cpp"
    "${LAUNCHER}/filterpopup.cpp" "${LAUNCHER}/filterpopup.h" "${LAUNCHER}/resources.qrc")
target_link_libraries(launcher-capture PRIVATE launcher_core Qt6::Widgets Qt6::Test)
set_target_properties(launcher-capture PROPERTIES WIN32_EXECUTABLE FALSE)
if(MSVC)
    target_compile_options(launcher-capture PRIVATE /utf-8)
endif()
