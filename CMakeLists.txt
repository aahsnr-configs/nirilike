cmake_minimum_required(VERSION 3.27)

project(nirilike
    DESCRIPTION "Niri-style vertical strip workspace overview plugin for Hyprland"
    VERSION 1.0
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Collect all .cpp sources automatically.
file(GLOB_RECURSE SOURCES "*.cpp")

add_library(nirilike SHARED ${SOURCES})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)

target_link_libraries(nirilike PRIVATE rt PkgConfig::deps)

target_compile_options(nirilike PRIVATE
    -Wall
    -Wextra
)

install(TARGETS nirilike)
