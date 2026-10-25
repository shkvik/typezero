# Every tests/run/*.tz is compiled and run twice: as usual (optimized, with
# the runtime bitcode linked into the program) and with -O0 (no optimization,
# the runtime library). One with async code runs a third time on threads,
# each piece that may run ahead of its turn on one of them (run/NAME/threads,
# docs/async.md, section 8). Every tests/errors/*.tz must fail to compile.
# See check.cmake for the expectations.
#
# A program of several files is a directory: tests/run/NAME/main.tz is its
# entry, with the expected output in main.out next to it, and
# tests/errors/NAME/main.tz is compiled with the files it imports, any of
# which may expect errors.
#
# Run tests marked with a `// same-as-node` line behave exactly as in
# JavaScript; when Node.js (23.6 or newer, which strips types) is installed,
# they also run under node against the same expected output. Options for
# node may follow, as in `// same-as-node --experimental-transform-types`
# for enums, which node does not strip. Node runs a copy of the program
# whose files end in .ts (check.cmake). Node needs the extension in an
# import: `from "./math.tz"`.

find_program(TZ_NODE node)

# The name of a test: its file, or the directory of a main.tz.
function(tz_test_name src out_name out_multi)
  get_filename_component(name ${src} NAME_WE)
  get_filename_component(dir ${src} DIRECTORY)
  get_filename_component(parent ${dir} NAME)
  if(name STREQUAL "main" AND NOT parent MATCHES "^(run|errors)$")
    set(${out_name} ${parent} PARENT_SCOPE)
    set(${out_multi} ON PARENT_SCOPE)
  else()
    set(${out_name} ${name} PARENT_SCOPE)
    set(${out_multi} OFF PARENT_SCOPE)
  endif()
endfunction()

file(GLOB TZ_RUN_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/run/*.tz
     ${CMAKE_CURRENT_SOURCE_DIR}/run/*/main.tz)
foreach(src ${TZ_RUN_TESTS})
  tz_test_name(${src} name multi)
  add_test(NAME run/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=run -DMULTI=${multi}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
  add_test(NAME run/${name}/O0
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=run -DOPT=-O0
            -DMULTI=${multi} -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
  file(STRINGS ${src} async_lines REGEX "(^|[^a-zA-Z_])(async|await)[^a-zA-Z_]")
  if(async_lines)
    add_test(NAME run/${name}/threads
      COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=run -DTHREADS=ON
              -DMULTI=${multi} -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
  endif()

  file(STRINGS ${src} same_as_node REGEX "^// same-as-node")
  if(TZ_NODE AND same_as_node)
    add_test(NAME node/${name}
      COMMAND ${CMAKE_COMMAND} -DNODE=${TZ_NODE} -DSRC=${src} -DMODE=node -DMULTI=${multi}
              -DCOPY=${CMAKE_CURRENT_BINARY_DIR}/node/${name}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
  endif()
endforeach()

# What runs in parallel (docs/async.md, section 5): a run test with
# NAME.explain next to it (main.explain for a program of several files) is
# also explained by `tz build --explain-parallel`, which must print exactly
# that.
file(GLOB TZ_EXPLAIN_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/run/*.explain
     ${CMAKE_CURRENT_SOURCE_DIR}/run/*/main.explain)
foreach(expected ${TZ_EXPLAIN_TESTS})
  get_filename_component(dir ${expected} DIRECTORY)
  get_filename_component(stem ${expected} NAME_WE)
  set(src ${dir}/${stem}.tz)
  tz_test_name(${src} name multi)
  add_test(NAME explain/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=explain
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
endforeach()

# Examples have no expected output; they only have to compile and exit with 0.
file(GLOB TZ_EXAMPLES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/examples/*.tz)
foreach(src ${TZ_EXAMPLES})
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME examples/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=run
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
endforeach()

# What a program of node:crypto links, and `tz licenses` of it.
add_test(NAME crypto/link
  COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/crypto_link
          -P ${CMAKE_CURRENT_SOURCE_DIR}/crypto_link.cmake)

file(GLOB TZ_ERROR_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/errors/*.tz
     ${CMAKE_CURRENT_SOURCE_DIR}/errors/*/main.tz)
foreach(src ${TZ_ERROR_TESTS})
  tz_test_name(${src} name multi)
  add_test(NAME errors/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=error -DMULTI=${multi}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
endforeach()

# Tests of tz's commands (tz init, add, remove, update, port; build and run of
# a project): tests/cli/NAME.cmake runs tz in a directory of its own with the
# helpers of cli.cmake. Packages come from local git repositories and a mirror
# of JSR on the disk that the test makes, never from the network.
find_program(TZ_GIT git)
# TypeScript's checker, for cli/types: its lib/typescript.js, by default
# the one VS Code comes with (looked for again once an update of VS Code has
# moved it).
set(TZ_TYPESCRIPT "" CACHE FILEPATH "lib/typescript.js of TypeScript, for tests/cli/types.cmake")
if(NOT EXISTS "${TZ_TYPESCRIPT}")
  find_program(TZ_VSCODE code)
  if(TZ_VSCODE)
    get_filename_component(vscode ${TZ_VSCODE} DIRECTORY)
    get_filename_component(vscode ${vscode} DIRECTORY)
    file(GLOB found ${vscode}/resources/app/extensions/node_modules/typescript/lib/typescript.js
                    ${vscode}/*/resources/app/extensions/node_modules/typescript/lib/typescript.js)
    if(found)
      list(GET found 0 found)
      set(TZ_TYPESCRIPT ${found} CACHE FILEPATH "lib/typescript.js of TypeScript, for tests/cli/types.cmake" FORCE)
    endif()
  endif()
endif()
file(GLOB TZ_CLI_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cmake)
foreach(src ${TZ_CLI_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME cli/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DGIT=${TZ_GIT} -DNODE=${TZ_NODE}
            -DTYPESCRIPT=${TZ_TYPESCRIPT}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cli -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli/${name}
            -P ${src})
  set_tests_properties(cli/${name} PROPERTIES SKIP_REGULAR_EXPRESSION "skipped: ")
endforeach()
