add_executable(tz
  main.cpp
  support/Diagnostics.cpp
  lex/Lexer.cpp
  parse/Parser.cpp
  sema/Types.cpp
  sema/Facts.cpp
  sema/Sema.cpp
  sema/SemaFlow.cpp
  sema/SemaBuiltins.cpp
  codegen/CodeGen.cpp
  codegen/CodeGenArray.cpp
  driver/Driver.cpp
  driver/Linker.cpp
)

set_target_properties(tz PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TZ_OUTPUT_DIR})

target_include_directories(tz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(tz SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(tz PRIVATE ${LLVM_DEFINITIONS_LIST} TZ_VERSION="${PROJECT_VERSION}")

llvm_map_components_to_libnames(TZ_LLVM_LIBS
  Core Support Analysis Passes Target TargetParser MC X86 AArch64)
target_link_libraries(tz PRIVATE ${TZ_LLVM_LIBS} lldCOFF lldCommon)

if(MSVC)
  # C4702 (unreachable code) fires inside inlined LLVM headers.
  target_compile_options(tz PRIVATE /W4 /wd4702 /utf-8 /permissive- /Zc:__cplusplus /GR- /EHs-c-)
  target_compile_definitions(tz PRIVATE _HAS_EXCEPTIONS=0)
else()
  target_compile_options(tz PRIVATE -Wall -Wextra -fno-rtti -fno-exceptions)
endif()

# Building tz also builds the runtime it links into every program.
add_dependencies(tz tzrt)
