cmake_minimum_required(VERSION 3.27)

project(nirilike
    DESCRIPTION "Niri-style vertical strip workspace overview plugin for Hyprland"
    VERSION 1.0
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export compile_commands.json so clangd / LSP tools can find all headers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Collect .cpp sources from the project root only — NOT recursively.
# GLOB_RECURSE would incorrectly sweep up CMake-generated files inside the
# build tree (e.g. CompilerIdCXX.cpp), causing spurious compilation errors.
file(GLOB SOURCES "*.cpp")

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
