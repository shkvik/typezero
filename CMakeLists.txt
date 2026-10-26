cmake_minimum_required(VERSION 3.25)

project(typezero VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
if(MSVC)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "LLVM is a Release build; configure with Release or RelWithDebInfo")
  endif()
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

find_package(zstd CONFIG REQUIRED)
find_package(LLVM REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

set(TZ_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin)

include(third_party/openssl/openssl.cmake)
file(COPY third_party/licenses/ DESTINATION ${TZ_OUTPUT_DIR}/licenses)
file(COPY third_party/types/node/ DESTINATION ${TZ_OUTPUT_DIR}/types/node_modules/@types/node)
file(COPY third_party/types/undici-types/ DESTINATION ${TZ_OUTPUT_DIR}/types/node_modules/undici-types)
configure_file(editors/types/tz.d.ts ${TZ_OUTPUT_DIR}/types/node_modules/@types/tz/index.d.ts
               COPYONLY)
configure_file(editors/types/tsconfig.json ${TZ_OUTPUT_DIR}/types/tsconfig.json COPYONLY)

add_subdirectory(runtime)
add_subdirectory(compiler)
add_subdirectory(library)

enable_testing()
add_subdirectory(tests)
