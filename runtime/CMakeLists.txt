# Gears3-Azura runtime: the recompiled game plus our own hooks, built as a native macOS executable.
#
# The recompiled game code in generated/ is produced from your own default.xex by `rexglue codegen`
# and is not part of this repository. scripts/build.sh runs the whole sequence.
#
# Our own code (runtime/src, with the SDK's host sources) is the object library azura_host, which
# needs no generated code: it builds without a default.xex, and the executable links it with the
# code generated from one. That split lets our code be built once and linked on another machine
# with the code generated there (docs/player-runtime-build.md).

cmake_minimum_required(VERSION 3.25)
project(azura LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# `rexglue codegen` writes this file. It declares the generated sources and the
# rexglue_setup_target() helper used below, so the executable cannot be configured before codegen
# has run. Without it, with the SDK built from source (REXSDK_DIR), the SDK and our own code are
# configured on their own: a first build of scripts/build.sh builds the patched code generator
# (target rexglue) there, runs codegen with it and configures again, and azura_host and the tests
# build without any game files.
set(AZURA_REXGLUE_CMAKE ${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake)
option(AZURA_KIT_PROBE "Configure the probes the player kit records its build commands from (scripts/build-kit.sh)" OFF)
if(AZURA_KIT_PROBE)
    # The player kit is built against the patched SDK as installed, the form it ships in, and
    # never with generated code, even where codegen has run.
    find_package(rexglue 0.10.0 CONFIG REQUIRED)
elseif(EXISTS ${AZURA_REXGLUE_CMAKE})
    include(${AZURA_REXGLUE_CMAKE})
elseif(REXSDK_DIR)
    add_subdirectory(${REXSDK_DIR} rexglue-sdk)
else()
    message(FATAL_ERROR
        "generated/rexglue.cmake not found. Put your default.xex in runtime/game/ and run "
        "scripts/build.sh, which runs `rexglue codegen` before configuring.")
endif()
# rexglue_add_host_sources() and the HOST_OBJECTS option come from our SDK patches.
if(NOT COMMAND rexglue_add_host_sources)
    message(FATAL_ERROR "The runtime needs the patched ReXGlue SDK source tree (REXSDK_DIR); see scripts/build.sh.")
endif()

set(AZURA_SOURCES
    src/main.cpp
    src/achievement_export.cpp
    src/ambient_occlusion.cpp
    src/achievement_toast.cpp
    src/hooks.cpp
    src/script_hooks.cpp
    src/class_dump.cpp
    src/render_queue.cpp
    src/relaunch.cpp
    src/shader_compile_overlay.cpp
    src/menu/coalesced.cpp
    src/input/install_player_assignment.cpp
    src/input/player_assignment.cpp
    src/menu/menu_patches.cpp
    src/menu/pc_options.cpp
    src/prompts/button_prompts.cpp
    src/prompts/glyph_art.cpp
    src/prompts/glyph_raster.cpp
    src/prompts/xenos_dxt5.cpp
)
if(APPLE)
    # platform_mac.mm is Objective-C++ (it sets the Dock icon through AppKit). It is compiled by the
    # C++ compiler in Objective-C++ mode rather than by enabling the OBJCXX language, which would
    # change how the SDK's bundled SDL builds its Objective-C files.
    list(APPEND AZURA_SOURCES src/platform_mac.mm)
    set_source_files_properties(src/platform_mac.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++")
endif()

# Keep the build machine's folder names out of the binary (they would otherwise end up in the
# __FILE__ strings of log messages).
add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)

# Our code and the SDK's host sources (its SDL entry point and ReXApp). An object library rather
# than a static archive: the executable gets every object, including the hooks that replace a
# function of the generated code by defining it again (the generated definition is weak), which a
# linker would leave out of an archive since the symbol is already defined.
add_library(azura_host OBJECT ${AZURA_SOURCES})
target_include_directories(azura_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(azura_host PRIVATE rex::runtime)
rexglue_add_host_sources(azura_host)

# With the SDK built from source (preset mac-arm64-sdk-source), the SDK's public headers include
# imgui.h, but the SDK does not pass imgui's folder on to its consumers the way the packaged SDK
# does (there, imgui.h sits in the SDK's include folder). Take it from the imgui target.
if(REXSDK_DIR AND TARGET imgui)
    target_include_directories(azura_host PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# Optional extra headers for the build machine (on Linux, the ALSA and PulseAudio headers a
# distribution may lack), in sdk/compat/include when present; nothing in the repository creates it.
set(AZURA_COMPAT_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/compat/include")
if(REXSDK_DIR AND EXISTS "${AZURA_COMPAT_INCLUDE}")
    target_include_directories(azura_host PRIVATE "${AZURA_COMPAT_INCLUDE}")
endif()

# Review tool for the button prompt designs: renders every glyph into a PNG without running the
# game. Not part of the normal build: cmake --build --preset <preset> --target azura_glyph_sheet
add_executable(azura_glyph_sheet EXCLUDE_FROM_ALL
    tools/glyph_sheet.cpp
    src/prompts/glyph_art.cpp
    src/prompts/glyph_raster.cpp
)

# Unit tests for the button prompt engine and the controller assignment (synthetic data only). Catch2 comes with the SDK source
# tree; it is added here on its own so the SDK's much larger test suite is not built.
# Run: ctest --test-dir out/build/mac-arm64-sdk-source -C RelWithDebInfo
if(REXSDK_DIR)
    if(NOT TARGET Catch2::Catch2WithMain)
        add_subdirectory(${REXSDK_DIR}/thirdparty/catch2 catch2 EXCLUDE_FROM_ALL)
    endif()
    enable_testing()
    add_executable(azura_prompts_tests
        tests/prompts_tests.cpp
        src/prompts/glyph_art.cpp
        src/prompts/glyph_raster.cpp
        src/prompts/xenos_dxt5.cpp
    )
    target_link_libraries(azura_prompts_tests PRIVATE Catch2::Catch2WithMain)
    add_test(NAME azura_prompts_tests COMMAND azura_prompts_tests)
    # Which controller plays which player: the policy alone, with the SDK's input headers.
    add_executable(azura_input_tests tests/input_tests.cpp src/input/player_assignment.cpp)
    target_include_directories(azura_input_tests PRIVATE src ${REXSDK_DIR}/include)
    target_link_libraries(azura_input_tests PRIVATE Catch2::Catch2WithMain)
    add_test(NAME azura_input_tests COMMAND azura_input_tests)
endif()

# The SDK stages MoltenVK's ICD file from ${REXGLUE_ROOT}/cmake, a variable only the packaged SDK
# defines. When the SDK is built from source, its source tree is that root.
if(REXSDK_DIR AND NOT REXGLUE_ROOT)
    set(REXGLUE_ROOT ${REXSDK_DIR})
endif()

# What a runtime executable needs besides the SDK's own set-up (rexglue_configure_target): AppKit
# on macOS, our patched MoltenVK, and our files beside it. Shared by azura and the kit probe, so
# the player kit stages exactly what this build does.
function(azura_configure_runtime target)
    if(APPLE)
        target_link_libraries(${target} PRIVATE "-framework Cocoa")
    endif()

    # The SDK copies MoltenVK into vulkan/lib in the target's POST_BUILD step, which only runs
    # when the target relinks. We patch MoltenVK (see scripts/build_moltenvk.sh and patches/), so
    # re-copy it on every build whether we are using the source SDK (MoltenVK::MoltenVK) or the
    # prebuilt SDK (rex::moltenvk).
    if(TARGET MoltenVK::MoltenVK)
        set(moltenvk MoltenVK::MoltenVK)
    elseif(TARGET rex::moltenvk)
        set(moltenvk rex::moltenvk)
    endif()
    if(moltenvk)
        add_custom_target(${target}_stage_moltenvk ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/vulkan/lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${moltenvk}>"
                "$<TARGET_FILE_DIR:${target}>/vulkan/lib/libMoltenVK.dylib"
            COMMENT "Staging MoltenVK next to the executable"
            VERBATIM)
        add_dependencies(${target}_stage_moltenvk ${target})
    endif()

    # The runtime loads its settings from azura.toml beside the executable. The documented
    # defaults are installed next to it as azura.default.toml on every build; the user's
    # azura.toml is never touched by the build.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/config/azura.default.toml
                $<TARGET_FILE_DIR:${target}>/azura.default.toml
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/config/gamecontrollerdb.txt
                $<TARGET_FILE_DIR:${target}>/gamecontrollerdb.txt
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/assets
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/azura-icon.png
                $<TARGET_FILE_DIR:${target}>/assets/azura-icon.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/azura-icon.icns
                $<TARGET_FILE_DIR:${target}>/assets/azura-icon.icns
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts
                $<TARGET_FILE_DIR:${target}>/assets/fonts
        COMMENT "Installing azura.default.toml, gamecontrollerdb.txt and assets beside the executable")
    if(APPLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND osascript -l JavaScript -e
                    "ObjC.import('AppKit');
                     var img = $.NSImage.alloc.initWithContentsOfFile('$<TARGET_FILE_DIR:${target}>/assets/azura-icon.png');
                     if (img) $.NSWorkspace.sharedWorkspace.setIconForFileOptions(img, '$<TARGET_FILE:${target}>', 0);"
            COMMENT "Setting macOS icon on the runtime executable"
            VERBATIM)
    endif()
endfunction()

# The probes of the player kit (scripts/build-kit.sh), which has to build the runtime on a machine
# with the player's generated code and no CMake. They are never built: the kit records the
# commands CMake would run for them. azura_kit_recomp is one source configured exactly as the
# generated code is (rexglue_add_recomp_library), and azura_kit_probe an executable configured
# exactly as azura is, linking azura_host and that source.
if(AZURA_KIT_PROBE)
    rexglue_add_recomp_library(azura_kit_recomp
        SOURCES tools/kit/recomp_probe.cpp
        INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tools/kit)
    add_executable(azura_kit_probe EXCLUDE_FROM_ALL)
    target_link_libraries(azura_kit_probe PRIVATE azura_kit_recomp)
    rexglue_configure_target(azura_kit_probe GPU_PLUGINS xenos HOST_OBJECTS azura_host)
    azura_configure_runtime(azura_kit_probe)
    if(TARGET azura_kit_probe_stage_moltenvk)
        set_target_properties(azura_kit_probe_stage_moltenvk PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()

# Everything below links our code with the generated code, which needs codegen to have run.
if(AZURA_KIT_PROBE OR NOT EXISTS ${AZURA_REXGLUE_CMAKE})
    message(STATUS "No generated code (or a kit probe build): only the SDK, our code (azura_host) "
                   "and the tests are configured")
    return()
endif()

# Every object comes from azura_host and the generated code (azura_recomp); the executable has no
# sources of its own.
add_executable(azura)

# GPU_PLUGINS xenos stages librexgpu-xenos.dylib next to the executable; on macOS the SDK also
# stages the Vulkan loader and MoltenVK. HOST_OBJECTS links azura_host instead of compiling the
# SDK's host sources into the executable again.
rexglue_setup_target(azura GPU_PLUGINS xenos HOST_OBJECTS azura_host)
azura_configure_runtime(azura)

if(REXSDK_DIR AND EXISTS "${AZURA_COMPAT_INCLUDE}" AND TARGET azura_recomp)
    target_include_directories(azura_recomp PRIVATE "${AZURA_COMPAT_INCLUDE}")
endif()
