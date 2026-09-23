cmake_minimum_required(VERSION 3.22.1)

project("vine_runtime" VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(
        -Wall -Wextra -Wpedantic -Wshadow
        -Wno-unused-parameter
        -Wno-gnu-zero-variadic-macro-arguments
        -fstack-protector-strong
        -DVINE_DEBUG=1
    )
else()
    add_compile_options(
        -Wall -Wextra
        -Wno-unused-parameter
        -O2
        -fstack-protector-strong
    )
endif()

set(VINE_SOURCES
    vine_runtime.cpp
    container/namespace_manager.cpp
    rootfs/ext4_probe.cpp
    qemu_bridge/qemu_launcher.cpp
    display/framebuffer_bridge.cpp
    input/uinput_bridge.cpp
    utils/vine_utils.cpp
    diagnostics/namespace_probe.cpp
)

add_library(vine_runtime SHARED ${VINE_SOURCES})

# libext2fs is cross-compiled by scripts/build-e2fsprogs-android.sh.
set(E2FS_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/e2fsprogs-android/${CMAKE_ANDROID_ARCH_ABI}")
set(E2FS_LIBDIR "${E2FS_PREFIX}/lib")
if(NOT EXISTS "${E2FS_LIBDIR}/libext2fs.a")
    message(FATAL_ERROR
        "Missing Android libext2fs for ${CMAKE_ANDROID_ARCH_ABI}. "
        "Run scripts/build-e2fsprogs-android.sh ${CMAKE_ANDROID_ARCH_ABI} first.")
endif()

foreach(lib ext2fs com_err uuid blkid)
    add_library(e2fs_${lib} STATIC IMPORTED)
    set_target_properties(e2fs_${lib} PROPERTIES
        IMPORTED_LOCATION "${E2FS_LIBDIR}/lib${lib}.a")
endforeach()

target_include_directories(vine_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/container
    ${CMAKE_CURRENT_SOURCE_DIR}/rootfs
    ${CMAKE_CURRENT_SOURCE_DIR}/qemu_bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/display
    ${CMAKE_CURRENT_SOURCE_DIR}/input
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    "${E2FS_PREFIX}/include"
)

find_library(log-lib     log)
find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)
find_library(z-lib z)

target_link_libraries(vine_runtime
    e2fs_ext2fs
    e2fs_com_err
    e2fs_uuid
    e2fs_blkid
    ${z-lib}
    ${log-lib}
    ${android-lib}
    ${jnigraphics-lib}
)

if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    target_compile_definitions(vine_runtime PRIVATE VINE_HOST_ARM64=1)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    target_compile_definitions(vine_runtime PRIVATE VINE_HOST_ARM32=1)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
    target_compile_definitions(vine_runtime PRIVATE VINE_HOST_X86_64=1)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86")
    target_compile_definitions(vine_runtime PRIVATE VINE_HOST_X86=1)
elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi")
    target_compile_definitions(vine_runtime PRIVATE VINE_HOST_ARMEABI=1)
endif()

if(ANDROID_PLATFORM_LEVEL LESS 26)
    message(FATAL_ERROR
        "VineOS requires Android API 26+ (minSdk=26). "
        "Current ANDROID_PLATFORM_LEVEL=${ANDROID_PLATFORM_LEVEL}")
endif()

if(NOT CMAKE_CXX_STANDARD EQUAL 17)
    message(FATAL_ERROR "VineOS native runtime requires C++17")
endif()

message(STATUS "VineOS native: ABI=${CMAKE_ANDROID_ARCH_ABI} "
               "API=${ANDROID_PLATFORM_LEVEL} "
               "BUILD=${CMAKE_BUILD_TYPE}")
