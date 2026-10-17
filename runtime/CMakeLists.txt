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

# Post-codegen patch: codegen writes azura_register.cpp as one function of ~57,000 registration
# calls, which takes clang close to ten minutes to compile (its cost grows faster than linearly
# with function size), and codegen rewrites it whenever a function hint is added.
# tools/split_register.py splits it into small functions (seconds to compile) without changing
# what is registered. It runs after every codegen, including the one the build itself re-runs, and
# before the generated code compiles.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(AZURA_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated/default)
set(AZURA_SPLIT_STAMP ${AZURA_GENERATED_DIR}/split_register.build.stamp)
add_custom_command(
    OUTPUT ${AZURA_SPLIT_STAMP}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/split_register.py
            ${AZURA_GENERATED_DIR}/azura_register.cpp
    COMMAND ${CMAKE_COMMAND} -E touch ${AZURA_SPLIT_STAMP}
    DEPENDS ${AZURA_GENERATED_DIR}/codegen.build.stamp
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/split_register.py
    COMMENT "Splitting the function registration into small functions"
    VERBATIM)
add_custom_target(azura_split_register DEPENDS ${AZURA_SPLIT_STAMP})
add_dependencies(azura_recomp azura_split_register)

# The runtime loads its settings from azura.toml beside the executable. The documented defaults
# are installed next to it as azura.default.toml on every build; the user's azura.toml is never
# touched by the build.
add_custom_command(TARGET azura POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/azura.default.toml
            $<TARGET_FILE_DIR:azura>/azura.default.toml
    COMMENT "Installing azura.default.toml beside the executable")
