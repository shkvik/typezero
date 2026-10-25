add_executable(tz
  main.cpp
  support/diagnostics.cpp
  support/decimal_literal.cpp
  support/regex_syntax.cpp
  support/date_syntax.cpp
  lex/lexer.cpp
  parse/parser.cpp
  sema/types.cpp
  sema/facts.cpp
  sema/sema.cpp
  sema/sema_flow.cpp
  sema/sema_builtins.cpp
  sema/sema_decls.cpp
  sema/sema_infer.cpp
  sema/sema_closure.cpp
  sema/sema_truth.cpp
  sema/sema_union.cpp
  sema/sema_predicate.cpp
  sema/sema_object.cpp
  sema/sema_process.cpp
  sema/sema_modules.cpp
  sema/sema_destination.cpp
  sema/sema_generic.cpp
  sema/sema_arglist.cpp
  sema/sema_generic_class.cpp
  sema/sema_decimal.cpp
  sema/sema_result.cpp
  sema/sema_class.cpp
  sema/sema_classvalue.cpp
  sema/sema_reflect.cpp
  sema/sema_inherit.cpp
  sema/sema_error.cpp
  sema/sema_iter.cpp
  sema/sema_iterable.cpp
  sema/sema_typeops.cpp
  sema/sema_conditional.cpp
  sema/sema_widen.cpp
  sema/sema_regex.cpp
  sema/sema_json.cpp
  sema/sema_date.cpp
  sema/sema_intl.cpp
  sema/sema_node.cpp
  sema/sema_child.cpp
  sema/sema_dict.cpp
  sema/sema_async.cpp
  sema/sema_bytes.cpp
  sema/sema_unknown.cpp
  sema/sema_decorate.cpp
  sema/overflow.cpp
  sema/parallel.cpp
  sema/sema_async_io.cpp
  sema/sema_events.cpp
  sema/sema_stream.cpp
  sema/sema_crypto.cpp
  sema/sema_library.cpp
  sema/sema_weak.cpp
  sema/sema_webcrypto.cpp
  codegen/codegen.cpp
  codegen/codegen_array.cpp
  codegen/codegen_builtins.cpp
  codegen/codegen_closure.cpp
  codegen/codegen_cycles.cpp
  codegen/codegen_iter.cpp
  codegen/codegen_object.cpp
  codegen/codegen_process.cpp
  codegen/codegen_decimal.cpp
  codegen/codegen_result.cpp
  codegen/codegen_union.cpp
  codegen/codegen_regex.cpp
  codegen/codegen_date.cpp
  codegen/codegen_regex_native.cpp
  codegen/codegen_reflect.cpp
  codegen/codegen_bytes.cpp
  codegen/codegen_intl.cpp
  codegen/codegen_tail.cpp
  codegen/codegen_class.cpp
  codegen/codegen_json.cpp
  codegen/codegen_inherit.cpp
  codegen/codegen_throw.cpp
  codegen/codegen_node.cpp
  codegen/codegen_child.cpp
  codegen/codegen_dict.cpp
  codegen/codegen_async.cpp
  codegen/codegen_unknown.cpp
  codegen/codegen_decorate.cpp
  codegen/codegen_async_io.cpp
  codegen/codegen_async_iter.cpp
  codegen/codegen_events.cpp
  codegen/codegen_stream.cpp
  codegen/codegen_crypto.cpp
  codegen/codegen_library.cpp
  driver/driver.cpp
  driver/licenses.cpp
  driver/linker.cpp
  driver/loader.cpp
  pkg/json.cpp
  pkg/semver.cpp
  pkg/net.cpp
  pkg/fetch.cpp
  pkg/scan.cpp
  pkg/project.cpp
  pkg/commands.cpp
  pkg/port.cpp
)

set_target_properties(tz PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TZ_OUTPUT_DIR})

target_include_directories(tz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The version of OpenSSL, which `tz licenses` names (driver/licenses.cpp).
target_include_directories(tz SYSTEM PRIVATE ${TZ_OPENSSL_DIR}/include)
target_include_directories(tz SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(tz PRIVATE ${LLVM_DEFINITIONS_LIST} TZ_VERSION="${PROJECT_VERSION}")

llvm_map_components_to_libnames(TZ_LLVM_LIBS
  Core Support Analysis Passes IPO BitReader Linker Target TargetParser MC Object X86 AArch64)
target_link_libraries(tz PRIVATE ${TZ_LLVM_LIBS} lldCOFF lldCommon)
if(WIN32)
  # Downloads from JSR (src/pkg/net.cpp).
  target_link_libraries(tz PRIVATE winhttp)
endif()

if(MSVC)
  # C4702 (unreachable code) fires inside inlined LLVM headers.
  target_compile_options(tz PRIVATE /W4 /wd4702 /utf-8 /permissive- /Zc:__cplusplus /GR- /EHs-c-)
  target_compile_definitions(tz PRIVATE _HAS_EXCEPTIONS=0)
else()
  target_compile_options(tz PRIVATE -Wall -Wextra -fno-rtti -fno-exceptions)
endif()

# Building tz also builds the runtime it links into every program.
add_dependencies(tz tzrt)

# The built-in modules written in tz go next to tz, where the loader reads
# them (src/driver/loader.cpp); copied again whenever they change.
foreach(file lib/events.ts lib/web.ts lib/url.ts lib/fetch.ts lib/http.ts lib/webcrypto.ts
             lib/stream.ts lib/stream_promises.ts lib/stream_consumers.ts lib/string_decoder.ts
             lib/fs_stream.ts)
  configure_file(${file} ${TZ_OUTPUT_DIR}/${file} COPYONLY)
endforeach()
