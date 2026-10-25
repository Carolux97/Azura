# Cross-builds the runtime for Windows (x86-64) from Linux, with clang targeting the MSVC ABI as the
# SDK's own Windows build does (clang with Microsoft's headers and libraries), for the preset
# win-amd64-cross-sdk-source.
#
# Microsoft's CRT and Windows SDK come from xwin's splat layout, the one cargo-xwin downloads for
# scripts/build-launcher-windows.sh (~/.cache/cargo-xwin/xwin, or XWIN_DIR): crt/ and sdk/, with the
# lowercase aliases a case-sensitive file system needs. The linker is rustup's rust-lld, which links
# as Microsoft's linker when called lld-link, so lld does not need to be installed.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(AZURA_WIN_TARGET x86_64-pc-windows-msvc)

# The newest LLVM with every tool the build needs.
file(GLOB azura_llvm_dirs /usr/lib/llvm-*/bin)
list(SORT azura_llvm_dirs COMPARE NATURAL ORDER DESCENDING)
foreach(dir IN LISTS azura_llvm_dirs)
    if(EXISTS ${dir}/clang++ AND EXISTS ${dir}/llvm-ar AND EXISTS ${dir}/llvm-rc AND EXISTS ${dir}/llvm-mt)
        set(AZURA_LLVM_BIN ${dir})
        break()
    endif()
endforeach()
if(NOT AZURA_LLVM_BIN)
    message(FATAL_ERROR "No LLVM with clang++, llvm-ar, llvm-rc and llvm-mt in /usr/lib/llvm-*/bin.")
endif()

if(DEFINED ENV{XWIN_DIR})
    set(AZURA_XWIN $ENV{XWIN_DIR})
else()
    set(AZURA_XWIN $ENV{HOME}/.cache/cargo-xwin/xwin)
endif()
if(NOT EXISTS ${AZURA_XWIN}/crt/include OR NOT EXISTS ${AZURA_XWIN}/sdk/include/um)
    message(FATAL_ERROR "Microsoft's CRT and Windows SDK not found in ${AZURA_XWIN}: run "
        "scripts/build-launcher-windows.sh once (it downloads them), or set XWIN_DIR.")
endif()

# rust-lld, linked as lld-link next to the xwin files.
execute_process(COMMAND rustc --print sysroot OUTPUT_VARIABLE azura_rust_sysroot OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND rustc -vV OUTPUT_VARIABLE azura_rust_info)
string(REGEX MATCH "host: ([^\n]+)" _ "${azura_rust_info}")
set(azura_rust_lld ${azura_rust_sysroot}/lib/rustlib/${CMAKE_MATCH_1}/bin/rust-lld)
if(NOT EXISTS ${azura_rust_lld})
    message(FATAL_ERROR "rust-lld not found at ${azura_rust_lld}; install Rust with rustup.")
endif()
set(AZURA_LLD_LINK ${AZURA_XWIN}/../lld-link)
file(CREATE_LINK ${azura_rust_lld} ${AZURA_LLD_LINK} SYMBOLIC)

set(CMAKE_C_COMPILER ${AZURA_LLVM_BIN}/clang)
set(CMAKE_CXX_COMPILER ${AZURA_LLVM_BIN}/clang++)
set(CMAKE_C_COMPILER_TARGET ${AZURA_WIN_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${AZURA_WIN_TARGET})
set(CMAKE_ASM_COMPILER_TARGET ${AZURA_WIN_TARGET})
set(CMAKE_RC_COMPILER ${AZURA_LLVM_BIN}/llvm-rc)
set(CMAKE_AR ${AZURA_LLVM_BIN}/llvm-ar)
set(CMAKE_MT ${AZURA_LLVM_BIN}/llvm-mt)
set(CMAKE_LINKER ${AZURA_LLD_LINK})

# Windows headers the sources include with a capitalisation xwin's splat has no alias for. Windows
# finds them whatever the case; here each gets a link in a folder searched after the SDK's.
set(azura_win_header_aliases ObjBase.h SDKDDKVer.h)
set(azura_alias_dir ${AZURA_XWIN}/../case-aliases)
file(MAKE_DIRECTORY ${azura_alias_dir})
foreach(header IN LISTS azura_win_header_aliases)
    string(TOLOWER ${header} lower)
    foreach(dir IN ITEMS sdk/include/um sdk/include/shared sdk/include/ucrt crt/include)
        if(EXISTS ${AZURA_XWIN}/${dir}/${lower})
            file(CREATE_LINK ${AZURA_XWIN}/${dir}/${lower} ${azura_alias_dir}/${header} SYMBOLIC)
            break()
        endif()
    endforeach()
endforeach()

set(azura_win_includes
    ${AZURA_XWIN}/crt/include
    ${AZURA_XWIN}/sdk/include/ucrt
    ${AZURA_XWIN}/sdk/include/um
    ${AZURA_XWIN}/sdk/include/shared
    ${AZURA_XWIN}/sdk/include/winrt
    ${azura_alias_dir})
# The CPU level of the Linux build. Kept here rather than in the preset: a CMAKE_C_FLAGS set there
# would replace these initial flags, include folders and all.
set(azura_win_flags "-march=x86-64-v3")
foreach(dir IN LISTS azura_win_includes)
    # What clang-cl's -imsvc does: a system folder searched after clang's own headers.
    string(APPEND azura_win_flags " -Xclang -internal-isystem -Xclang ${dir}")
endforeach()
set(CMAKE_C_FLAGS_INIT "${azura_win_flags}")
set(CMAKE_CXX_FLAGS_INIT "${azura_win_flags}")
# llvm-rc finds the Windows SDK headers the resource scripts include through these.
set(CMAKE_RC_FLAGS_INIT "-I ${AZURA_XWIN}/sdk/include/um -I ${AZURA_XWIN}/sdk/include/shared")

# CMake adds -fuse-ld=lld-link itself; -B makes clang look for lld-link where the link to rust-lld is.
get_filename_component(azura_lld_dir ${AZURA_LLD_LINK} DIRECTORY)
set(azura_win_link "-fuse-ld=lld-link -B${azura_lld_dir}")
foreach(dir IN ITEMS ${AZURA_XWIN}/crt/lib/x86_64 ${AZURA_XWIN}/sdk/lib/um/x86_64 ${AZURA_XWIN}/sdk/lib/ucrt/x86_64)
    string(APPEND azura_win_link " -Xlinker /libpath:${dir}")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${azura_win_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${azura_win_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${azura_win_link}")

# Programs (the code generator's helpers) come from the build machine, libraries and headers from
# the target.
set(CMAKE_FIND_ROOT_PATH ${AZURA_XWIN})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# xwin's splat has the release CRT only (no msvcrtd.lib), so every configuration, and CMake's own
# compiler checks, use it.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
