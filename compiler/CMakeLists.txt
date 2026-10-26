file(GLOB_RECURSE TZ_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE TZ_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_executable(tz ${TZ_SOURCES} ${TZ_HEADERS})

set_target_properties(tz PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TZ_OUTPUT_DIR})

target_include_directories(tz PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(tz SYSTEM PRIVATE ${TZ_OPENSSL_DIR}/include)
target_include_directories(tz SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(tz PRIVATE ${LLVM_DEFINITIONS_LIST} TZ_VERSION="${PROJECT_VERSION}")

llvm_map_components_to_libnames(TZ_LLVM_LIBS
  Core Support Analysis Passes IPO BitReader Linker Target TargetParser MC Object X86 AArch64)
target_link_libraries(tz PRIVATE ${TZ_LLVM_LIBS} lldCOFF lldCommon)
if(WIN32)
  target_link_libraries(tz PRIVATE winhttp)
endif()

if(MSVC)
  target_compile_options(tz PRIVATE /W4 /wd4702 /utf-8 /permissive- /Zc:__cplusplus /GR- /EHs-c-)
  target_compile_definitions(tz PRIVATE _HAS_EXCEPTIONS=0)
else()
  target_compile_options(tz PRIVATE -Wall -Wextra -fno-rtti -fno-exceptions)
endif()

add_dependencies(tz tzrt)
