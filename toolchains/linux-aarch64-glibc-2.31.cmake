cmake_minimum_required(VERSION 3.16)

if((NOT DEFINED TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT OR
    TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT STREQUAL "") AND
   DEFINED CMAKE_SYSROOT AND NOT CMAKE_SYSROOT STREQUAL "")
    set(TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT "${CMAKE_SYSROOT}")
endif()
if((NOT DEFINED TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT OR
    TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT STREQUAL "") AND
   DEFINED CMAKE_CXX_COMPILER AND NOT CMAKE_CXX_COMPILER STREQUAL "")
    get_filename_component(_testtools_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_testtools_toolchain_usr "${_testtools_compiler_bin}" DIRECTORY)
    get_filename_component(TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT
        "${_testtools_toolchain_usr}" DIRECTORY)
endif()
if(NOT DEFINED TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT OR
   TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT STREQUAL "")
    message(FATAL_ERROR
        "TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT must point to the extracted Debian Bullseye toolchain")
endif()

file(TO_CMAKE_PATH "${TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT}" _testtools_toolchain_root)
set(_testtools_triplet aarch64-linux-gnu)
set(_testtools_gcc "${_testtools_toolchain_root}/usr/bin/${_testtools_triplet}-gcc-10")
set(_testtools_gxx "${_testtools_toolchain_root}/usr/bin/${_testtools_triplet}-g++-10")

if(NOT EXISTS "${_testtools_gcc}" OR NOT EXISTS "${_testtools_gxx}")
    message(FATAL_ERROR "aarch64 GCC 10 compilers were not found below ${_testtools_toolchain_root}")
endif()
if(NOT EXISTS "${_testtools_toolchain_root}/usr/aarch64-linux-gnu/include")
    message(FATAL_ERROR "The glibc sysroot was not found below ${_testtools_toolchain_root}")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT "${_testtools_toolchain_root}")
set(CMAKE_C_COMPILER "${_testtools_gcc}")
set(CMAKE_CXX_COMPILER "${_testtools_gxx}")
set(CMAKE_C_COMPILER_TARGET ${_testtools_triplet})
set(CMAKE_CXX_COMPILER_TARGET ${_testtools_triplet})
set(CMAKE_C_FLAGS_INIT "--sysroot=${_testtools_toolchain_root}")
set(CMAKE_CXX_FLAGS_INIT "--sysroot=${_testtools_toolchain_root}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--sysroot=${_testtools_toolchain_root}")
set(CMAKE_FIND_ROOT_PATH "${_testtools_toolchain_root}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The Debian cross packages place host-side binutils libraries beside the
# extracted tools. The WSL wrapper exports LD_LIBRARY_PATH before CMake runs.
set(TESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ID
    "debian-bullseye-gcc-10.2.1-glibc-2.31" CACHE STRING "Toolchain identity")
