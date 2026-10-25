cmake_minimum_required(VERSION 3.25)

# The runtime (C) is built by clang through custom commands: see runtime/.
project(typezero VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The prebuilt LLVM is a Release build with the static CRT (/MT), no RTTI and no
# C++ exceptions. Everything linked with it has to match.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
if(MSVC)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "LLVM is a Release build; configure with Release or RelWithDebInfo")
  endif()
  # CMake enables exceptions (/EHsc) by default; src/CMakeLists.txt turns them off.
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# The prebuilt LLVM links zlib, zstd and libxml2 without shipping them; vcpkg
# provides them (see vcpkg.json). LLVM's own Findzstd looks for MSVC's static
# zstd under the name zstd_static.lib, so load vcpkg's package first: it
# defines the zstd::libzstd_static target LLVM links against.
find_package(zstd CONFIG REQUIRED)
find_package(LLVM REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

# tz looks for the runtime files next to its own executable.
set(TZ_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin)

include(third_party/openssl/openssl.cmake)
# The texts `tz licenses` prints, next to tz.exe (src/driver/licenses.cpp).
file(COPY third_party/licenses/ DESTINATION ${TZ_OUTPUT_DIR}/licenses)
add_subdirectory(runtime)
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
