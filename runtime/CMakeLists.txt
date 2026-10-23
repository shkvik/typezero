# The runtime, built by clang of the same LLVM that tz is built with, in two
# forms that land next to tz.exe, where tz looks for them:
#
#   tzrt.bc   LLVM bitcode. tz copies the small functions a program calls
#             from it into the program before optimizing, so that their fast
#             paths are inlined into the program (see linkRuntime in
#             src/driver/driver.cpp).
#   tzrt.lib  the same code compiled, linked into every program: everything
#             that was not inlined is called here.
#
# CMake cannot build C with clang while tz itself is built with MSVC, so both
# come from custom commands.

find_program(TZ_CLANG clang PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH REQUIRED)
find_program(TZ_LLVM_LINK llvm-link PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH REQUIRED)
find_program(TZ_LLVM_LIB llvm-lib PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH REQUIRED)

set(sources alloc.c tzrt.c string.c array.c inspect.c number.c math.c unicode.c closure.c)
set(flags -O2 -std=c17 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS)
if(WIN32)
  list(APPEND flags -fms-runtime-lib=static) # the static CRT, as tz links it
endif()

# Each file to bitcode. -flto=full stops before the optimizations that only
# pay off once everything is in one module (vectorization, unrolling).
set(dir ${CMAKE_CURRENT_BINARY_DIR}/bitcode)
file(MAKE_DIRECTORY ${dir})
set(bitcode_files)
foreach(source ${sources})
  get_filename_component(name ${source} NAME_WE)
  set(bitcode ${dir}/${name}.bc)
  add_custom_command(OUTPUT ${bitcode}
    COMMAND ${TZ_CLANG} ${flags} -flto=full -c -MD -MF ${bitcode}.d -o ${bitcode}
            ${CMAKE_CURRENT_SOURCE_DIR}/${source}
    DEPENDS ${source}
    DEPFILE ${bitcode}.d
    COMMENT "Compiling ${source} to bitcode"
    VERBATIM)
  list(APPEND bitcode_files ${bitcode})
endforeach()

# One module for the whole runtime: tzrt.bc, and compiled as a whole into
# tzrt.lib, so that the files' functions are inlined into each other too.
# Each function gets its own section, so the linker drops the unused ones.
add_custom_command(OUTPUT ${TZ_OUTPUT_DIR}/tzrt.bc
  COMMAND ${TZ_LLVM_LINK} -o ${TZ_OUTPUT_DIR}/tzrt.bc ${bitcode_files}
  DEPENDS ${bitcode_files}
  COMMENT "Linking the runtime bitcode"
  VERBATIM)
add_custom_command(OUTPUT ${TZ_OUTPUT_DIR}/tzrt.lib
  COMMAND ${TZ_CLANG} -O2 -ffunction-sections -fdata-sections -c -o ${dir}/tzrt.obj
          ${TZ_OUTPUT_DIR}/tzrt.bc
  COMMAND ${TZ_LLVM_LIB} /nologo /out:${TZ_OUTPUT_DIR}/tzrt.lib ${dir}/tzrt.obj
  DEPENDS ${TZ_OUTPUT_DIR}/tzrt.bc
  COMMENT "Compiling the runtime library"
  VERBATIM)
add_custom_target(tzrt DEPENDS ${TZ_OUTPUT_DIR}/tzrt.bc ${TZ_OUTPUT_DIR}/tzrt.lib
  SOURCES ${sources} tzrt.h unicode_tables.h gen_unicode_tables.mjs)
