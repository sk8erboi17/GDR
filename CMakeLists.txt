cmake_minimum_required(VERSION 3.25)

if(CMAKE_HOST_APPLE)
    # GRD's macOS backend deliberately targets ScreenCaptureKit's modern
    # streaming API. Keep the Mach-O load command aligned with Info.plist so
    # an app built on a newer SDK still runs on every supported macOS 14+
    # system and never advertises unreachable 12/13 compatibility.
    set(
        CMAKE_OSX_DEPLOYMENT_TARGET "14.0"
        CACHE STRING "Minimum supported macOS version" FORCE
    )
endif()

project(GRD VERSION 0.2.9 LANGUAGES C)

if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GRD_BUILD_TESTS "Build GRD unit tests" ON)
option(GRD_ENABLE_CUDA "Build the CUDA preprocessing backend when nvcc is available" ON)
option(
    GRD_REQUIRE_CUDA
    "Fail configuration instead of silently selecting the CUDA stub"
    OFF
)

if(GRD_REQUIRE_CUDA)
    set(GRD_ENABLE_CUDA ON CACHE BOOL "Build the CUDA preprocessing backend" FORCE)
endif()

include(FetchContent)
include(CheckLanguage)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3>=3.4.0)
if(TARGET PkgConfig::SDL3)
    if(WIN32 AND
       CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # The MSYS2 SDL3 pkg-config file exports the GNU driver option
        # -mwindows. clang-cl forwards it unchanged to link.exe, which emits
        # LNK4044. The GRD executable selects its Windows subsystem explicitly
        # below, so remove only this incompatible transitive option.
        get_property(
            grd_sdl_link_options
            TARGET PkgConfig::SDL3
            PROPERTY INTERFACE_LINK_OPTIONS
        )
        if(grd_sdl_link_options)
            list(REMOVE_ITEM grd_sdl_link_options "-mwindows")
            set_property(
                TARGET PkgConfig::SDL3
                PROPERTY INTERFACE_LINK_OPTIONS "${grd_sdl_link_options}"
            )
        endif()
    endif()
    set(GRD_SDL_TARGET PkgConfig::SDL3)
else()
    message(STATUS "System SDL3 not found; building the pinned SDL3 source")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        sdl3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.10
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(sdl3)
    set(GRD_SDL_TARGET SDL3::SDL3)
endif()
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavutil
    libswresample
    libswscale
)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

FetchContent_Declare(
    nuklear
    GIT_REPOSITORY https://github.com/Immediate-Mode-UI/Nuklear.git
    GIT_TAG v4.13.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nuklear)

set(GRD_CORE_SOURCES
    src/core/auth.c
    src/core/config.c
    src/core/discovery.c
    src/core/log.c
    src/core/protocol.c
    src/core/remote_access.c
    src/core/stream_policy.c
    src/core/transport.c
    src/platform/clipboard_sdl.c
    src/video/audio.c
    src/video/codec.c
    src/video/gpu.c
)

if(APPLE)
    list(APPEND GRD_CORE_SOURCES
        src/platform/macos.m
        src/platform/unix_remote.c
        src/video/metal.m
        src/video/videotoolbox.m
    )
elseif(WIN32)
    list(APPEND GRD_CORE_SOURCES
        src/platform/windows.c
        src/platform/unix_remote_stub.c
        src/video/metal_stub.c
    )
else()
    pkg_check_modules(
        X11_DEPS REQUIRED IMPORTED_TARGET
        x11 xtst xrandr xfixes xext libpulse-simple
    )
    list(APPEND GRD_CORE_SOURCES
        src/platform/linux_x11.c
        src/platform/unix_remote.c
        src/video/metal_stub.c
    )
endif()

if(GRD_ENABLE_CUDA)
    check_language(CUDA)
endif()
if(GRD_REQUIRE_CUDA AND NOT CMAKE_CUDA_COMPILER)
    message(FATAL_ERROR
        "GRD_REQUIRE_CUDA=ON but nvcc was not detected; refusing to build "
        "cuda_stub.c under a CUDA verification job"
    )
endif()
if(GRD_ENABLE_CUDA AND CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    if(MSVC OR CMAKE_CUDA_HOST_COMPILER_ID STREQUAL "MSVC")
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            # Hybrid (GNU C + MSVC-hosted CUDA): drop the MinGW implicit link
            # libraries so the MSVC-style final link does not receive
            # GNU-only runtime libs.
            set(CMAKE_C_IMPLICIT_LINK_LIBRARIES "")
        endif()
        # The device kernels are self-contained (fatbin embedded at compile
        # time) and cudart is linked explicitly via CUDA::cudart below, so
        # disable CMake's CUDA runtime library options to keep link.exe from
        # receiving extension-less library names.
        set(CMAKE_CUDA_RUNTIME_LIBRARY NONE)
    endif()
    list(APPEND GRD_CORE_SOURCES src/video/cuda_pipeline.cu)
    if(WIN32)
        list(APPEND GRD_CORE_SOURCES src/video/cuda_d3d11.cu)
    endif()
    set(GRD_HAS_CUDA 1)
    message(STATUS "GRD CUDA backend: enabled (${CMAKE_CUDA_COMPILER})")
else()
    list(APPEND GRD_CORE_SOURCES src/video/cuda_stub.c)
    set(GRD_HAS_CUDA 0)
    message(STATUS "GRD CUDA backend: disabled; using cuda_stub.c")
endif()

add_library(grd_core STATIC ${GRD_CORE_SOURCES})
target_include_directories(grd_core
    PUBLIC include
    PRIVATE src
)
target_compile_definitions(grd_core
    PUBLIC GRD_VERSION="${PROJECT_VERSION}"
    PRIVATE _POSIX_C_SOURCE=200809L
    PRIVATE GRD_HAS_CUDA=${GRD_HAS_CUDA}
)
if(APPLE)
    target_compile_definitions(grd_core PRIVATE _DARWIN_C_SOURCE)
elseif(UNIX)
    target_compile_definitions(grd_core PRIVATE _DEFAULT_SOURCE)
endif()
target_link_libraries(grd_core
    PUBLIC
        PkgConfig::FFMPEG
        ${GRD_SDL_TARGET}
        PkgConfig::SODIUM
        Threads::Threads
)

if(APPLE)
    target_link_libraries(grd_core PUBLIC
        "-framework ApplicationServices"
        "-framework AppKit"
        "-framework AudioToolbox"
        "-framework CoreGraphics"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework Metal"
        "-framework ScreenCaptureKit"
        "-framework VideoToolbox"
    )
    target_compile_options(grd_core PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
    )
elseif(WIN32)
    # GRD deliberately uses portable C runtime APIs such as strerror. MSVC
    # marks them deprecated in favor of non-standard *_s functions.
    target_compile_definitions(grd_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC OR CMAKE_CUDA_HOST_COMPILER_ID STREQUAL "MSVC")
        # Absolute paths survive the GNU link-item formatting untouched; bare
        # ".lib" names would be rewritten and extension-less names would be
        # treated as .obj files by link.exe. The LIB environment (vcvars)
        # points at the Windows SDK library directories.
        foreach(grd_sys_lib IN ITEMS bcrypt crypt32 d3d11 dxgi iphlpapi ole32 user32 ws2_32)
            find_library(
                GRD_SYS_${grd_sys_lib}
                NAMES ${grd_sys_lib}
                PATHS ENV LIB
                NO_DEFAULT_PATH
            )
            if(GRD_SYS_${grd_sys_lib})
                target_link_libraries(grd_core PUBLIC "${GRD_SYS_${grd_sys_lib}}")
            endif()
        endforeach()
    else()
        target_link_libraries(grd_core PUBLIC
            bcrypt crypt32 d3d11 dxgi
            iphlpapi ole32 user32 ws2_32
        )
    endif()
    if(GRD_HAS_CUDA)
        target_link_libraries(grd_core PUBLIC CUDA::cudart)
    endif()
else()
    target_link_libraries(grd_core PUBLIC PkgConfig::X11_DEPS dl)
endif()

if(MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(grd_core PRIVATE $<$<COMPILE_LANGUAGE:C>:/W4>)
else()
    target_compile_options(grd_core PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
    )
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND WIN32)
    # clang-cl builds (with the MSVC-hosted CUDA backend) consume the
    # FFmpeg/SDL3/libsodium headers through a shim containing package headers
    # only. The full MinGW prefix would shadow MSVC/UCRT headers (stdlib.h,
    # immintrin.h, etc.); mark the safe shim as external so warnings remain
    # focused on GRD sources.
    set(GRD_DEP_INCLUDE_SHIM "$ENV{GRD_DEP_INCLUDE}")
    set(GRD_MINGW_PREFIX_INCLUDE "")
    foreach(grd_pkg IN ITEMS PkgConfig::FFMPEG PkgConfig::SDL3 PkgConfig::SODIUM)
        if(TARGET "${grd_pkg}")
            get_target_property(grd_pkg_inc "${grd_pkg}" INTERFACE_INCLUDE_DIRECTORIES)
            if(grd_pkg_inc)
                foreach(grd_dir IN LISTS grd_pkg_inc)
                    if(IS_DIRECTORY "${grd_dir}")
                        set(GRD_MINGW_PREFIX_INCLUDE "${grd_dir}")
                    endif()
                endforeach()
                list(REMOVE_ITEM grd_pkg_inc "${GRD_MINGW_PREFIX_INCLUDE}")
                set_target_properties(
                    "${grd_pkg}"
                    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${grd_pkg_inc}"
                )
            endif()
            # The MinGW prefix also ships static archives (libavcodec.a etc.)
            # that pull in VAAPI/QSV/X11 dependencies. Point the MSVC-style
            # link at the import libraries (.dll.a) so only the runtime DLLs
            # are required.
            get_target_property(grd_pkg_libs "${grd_pkg}" INTERFACE_LINK_LIBRARIES)
            if(grd_pkg_libs)
                set(grd_pkg_libs_new "")
                foreach(grd_lib IN LISTS grd_pkg_libs)
                    if(grd_lib MATCHES "^(.*/)lib([^/]+)\\.a$")
                        list(APPEND grd_pkg_libs_new
                            "${CMAKE_MATCH_1}lib${CMAKE_MATCH_2}.dll.a"
                        )
                    else()
                        list(APPEND grd_pkg_libs_new "${grd_lib}")
                    endif()
                endforeach()
                set_target_properties(
                    "${grd_pkg}"
                    PROPERTIES INTERFACE_LINK_LIBRARIES "${grd_pkg_libs_new}"
                )
            endif()
        endif()
    endforeach()
    if(GRD_DEP_INCLUDE_SHIM)
        set(GRD_MINGW_PREFIX_INCLUDE "${GRD_DEP_INCLUDE_SHIM}")
    endif()
    if(GRD_MINGW_PREFIX_INCLUDE)
        target_include_directories(
            grd_core SYSTEM PRIVATE "${GRD_MINGW_PREFIX_INCLUDE}"
        )
    endif()
    # SDL detects CPU features at runtime; disable the compile-time intrinsics
    # macros so SDL never includes the conflicting intrinsic headers.
    add_compile_definitions(
        SDL_DISABLE_SSE SDL_DISABLE_SSE2 SDL_DISABLE_SSE3
        SDL_DISABLE_SSE4_1 SDL_DISABLE_SSE4_2
        SDL_DISABLE_AVX SDL_DISABLE_AVX2 SDL_DISABLE_AVX512F
    )
endif()

if(CMAKE_CUDA_COMPILER_ID STREQUAL "NVIDIA" AND WIN32)
    # NVCC's device front end diagnoses valid declarations in the Windows SDK
    # headers. Scope these suppressions to CUDA compilation; host C warnings
    # remain enabled at /W4.
    target_compile_options(grd_core PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=68>
        $<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=108>
        $<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=174>
        $<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=1835>
    )
endif()

add_executable(grd
    src/app/main.c
    src/app/ui.c
)
if(GRD_HAS_CUDA)
    # The final link must follow the MSVC-style CUDA rules (host compiler
    # cl/link.exe), not the GNU C rules, so library names and CRT libs stay
    # compatible with the CUDA objects.
    set_target_properties(grd PROPERTIES LINKER_LANGUAGE CUDA)
endif()
target_include_directories(grd PRIVATE
    include
    src
    ${nuklear_SOURCE_DIR}
    ${nuklear_SOURCE_DIR}/demo/sdl3_renderer
)
if(GRD_MINGW_PREFIX_INCLUDE)
    target_include_directories(
        grd SYSTEM PRIVATE "${GRD_MINGW_PREFIX_INCLUDE}"
    )
endif()
target_link_libraries(grd PRIVATE grd_core ${GRD_SDL_TARGET})

if(APPLE)
    set(
        GRD_CODESIGN_IDENTITY "-" CACHE STRING
        "macOS signing identity; use '-' for a stable ad-hoc development signature"
    )
    set_target_properties(grd PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "GRD"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.grd.GRD"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
    )
    set(grd_codesign_requirements)
    if(GRD_CODESIGN_IDENTITY STREQUAL "-")
        list(APPEND grd_codesign_requirements
            --requirements
            [=[=designated => identifier "dev.grd.GRD"]=]
        )
    endif()
    add_custom_command(TARGET grd POST_BUILD
        COMMAND /usr/bin/codesign
            --force --deep
            --sign "${GRD_CODESIGN_IDENTITY}"
            ${grd_codesign_requirements}
            --identifier dev.grd.GRD
            "$<TARGET_BUNDLE_DIR:grd>"
        VERBATIM
        COMMENT "Signing GRD.app with a stable code requirement"
    )
elseif(WIN32)
    if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # CMake 4.4 emits the GNU-style -mwindows flag for this clang-cl
        # configuration even though it invokes link.exe directly. Select the
        # GUI subsystem explicitly and keep the portable main() entry point.
        target_link_options(grd PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        set_target_properties(grd PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    # Nuklear intentionally uses the portable C stdio API, which MSVC marks
    # deprecated in favor of non-standard *_s functions unless opted out.
    target_compile_definitions(grd PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS grd
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(APPLE)
    install(CODE
        "set(grd_codesign_identity [[${GRD_CODESIGN_IDENTITY}]])"
    )
    install(CODE [[
        include(BundleUtilities)
        get_filename_component(
            grd_install_root
            "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}"
            ABSOLUTE
        )
        set(grd_installed_bundle "${grd_install_root}/grd.app")
        fixup_bundle("${grd_installed_bundle}" "" "")
        set(grd_codesign_command
            /usr/bin/codesign
            --force --deep
            --sign "${grd_codesign_identity}"
            --identifier dev.grd.GRD
        )
        if(grd_codesign_identity STREQUAL "-")
            list(APPEND grd_codesign_command
                --requirements
                "=designated => identifier \"dev.grd.GRD\""
            )
        endif()
        list(APPEND grd_codesign_command "${grd_installed_bundle}")
        execute_process(
            COMMAND ${grd_codesign_command}
            COMMAND_ERROR_IS_FATAL ANY
        )
    ]])
elseif(UNIX)
    install(
        FILES packaging/linux/grd.desktop
        DESTINATION share/applications
    )
    install(
        FILES packaging/linux/grd.svg
        DESTINATION share/icons/hicolor/scalable/apps
    )
endif()

if(GRD_BUILD_TESTS)
    enable_testing()
    add_executable(grd_tests
        tests/test_main.c
        tests/test_auth.c
        tests/test_audio.c
        tests/test_codec.c
        tests/test_protocol.c
        tests/test_remote_access.c
        tests/test_stream_policy.c
        tests/test_transport.c
    )
    if(GRD_HAS_CUDA)
        set_target_properties(grd_tests PROPERTIES LINKER_LANGUAGE CUDA)
    endif()
    target_include_directories(grd_tests PRIVATE include src tests)
    target_link_libraries(grd_tests PRIVATE grd_core)
    if(GRD_MINGW_PREFIX_INCLUDE)
        target_include_directories(
            grd_tests SYSTEM PRIVATE "${GRD_MINGW_PREFIX_INCLUDE}"
        )
    endif()
    add_test(NAME grd_tests COMMAND grd_tests)
    if(WIN32)
        set(grd_windows_powershell
            "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
        )
        if(EXISTS "${grd_windows_powershell}")
            add_test(
                NAME grd_remote_access_policy
                COMMAND "${grd_windows_powershell}"
                    -NoLogo
                    -NoProfile
                    -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/scripts/grd-remote-access.ps1"
                    -Mode SelfTest
            )
        endif()
    endif()
endif()

# Headless peer-network diagnostic client: connects to a GRD host, receives
# the real video stream and reports rx/assembly/decode/SPS-PPS diagnostics.
# Builds everywhere grd_core builds; not part of the app bundle.
add_executable(grd_diag src/tools/grd_diag.c)
target_include_directories(grd_diag PRIVATE include)
target_link_libraries(grd_diag PRIVATE grd_core)
if(GRD_MINGW_PREFIX_INCLUDE)
    target_include_directories(
        grd_diag SYSTEM PRIVATE "${GRD_MINGW_PREFIX_INCLUDE}"
    )
endif()

set(CPACK_PACKAGE_NAME "GRD")
set(CPACK_PACKAGE_VENDOR "sk8erboi17")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "LAN-only remote desktop with video, input, clipboard and audio")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "gperrotta03@gmail.com")

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Giuseppe Perrotta")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "xfce4")
endif()

include(CPack)
