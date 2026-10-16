# Gears3-Azura runtime: the recompiled game plus our own hooks, built as a native macOS executable.
#
# The recompiled game code in generated/ is produced from your own default.xex by `rexglue codegen`
# and is not part of this repository. scripts/build.sh runs the whole sequence.

cmake_minimum_required(VERSION 3.25)
project(azura LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# `rexglue codegen` writes this file. It declares the generated sources and the
# rexglue_setup_target() helper used below, so nothing can be configured before codegen has run.
set(AZURA_REXGLUE_CMAKE ${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake)
if(NOT EXISTS ${AZURA_REXGLUE_CMAKE})
    message(FATAL_ERROR
        "generated/rexglue.cmake not found. Put your default.xex in runtime/game/ and run "
        "scripts/build.sh, which runs `rexglue codegen` before configuring.")
endif()
include(${AZURA_REXGLUE_CMAKE})

set(AZURA_SOURCES
    src/main.cpp
)

# Keep the build machine's folder names out of the binary (they would otherwise end up in the
# __FILE__ strings of log messages).
add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)

add_executable(azura ${AZURA_SOURCES})

# GPU_PLUGINS xenos stages librexgpu-xenos.dylib next to the executable; on macOS the SDK also
# stages the Vulkan loader and MoltenVK.
rexglue_setup_target(azura GPU_PLUGINS xenos)

# The runtime loads its settings from azura.toml beside the executable. The documented defaults
# are installed next to it as azura.default.toml on every build; the user's azura.toml is never
# touched by the build.
add_custom_command(TARGET azura POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/azura.default.toml
            $<TARGET_FILE_DIR:azura>/azura.default.toml
    COMMENT "Installing azura.default.toml beside the executable")
