# The modules of tz's own library, declared for TypeScript's checker in the
# editor (docs/modules.md, section 7): each .d.tz of the library becomes a
# `declare module` next to tz.d.ts in @types/tz, which tz.d.ts references.
# A .d.tz is already TypeScript but for two things: inside `declare module`
# its classes and functions are not `declare` again, and the names it imports
# from tz's inner modules (tz:fetch, tz:url, tz:web) are globals for
# TypeScript.

# declare_module(<file.d.tz> <module> <file.d.ts>)
function(declare_module source module out)
  file(READ ${source} text)
  string(REPLACE "\r\n" "\n" text "${text}")
  string(REGEX REPLACE "import [^;]*from \"tz:(fetch|url|web)\";\n" "" text "${text}")
  string(REPLACE "export declare " "export " text "${text}")
  get_filename_component(name ${source} NAME)
  file(WRITE ${out} "// Made from library/.../${name} by editors/types/declarations.cmake.\n"
                    "declare module \"${module}\" {\n${text}}\n")
endfunction()

set(types_tz ${TZ_OUTPUT_DIR}/types/node_modules/@types/tz)
declare_module(${CMAKE_CURRENT_SOURCE_DIR}/library/http/http.d.tz "tz:http" ${types_tz}/http.d.ts)
declare_module(${CMAKE_CURRENT_SOURCE_DIR}/library/http/http2.d.tz "tz:http2" ${types_tz}/http2.d.ts)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/library/http/http.d.tz
             ${CMAKE_CURRENT_SOURCE_DIR}/library/http/http2.d.tz)
