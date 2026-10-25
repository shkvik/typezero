# Every tests/run/*.ts is compiled and run twice: as usual (optimized, with
# the runtime bitcode linked into the program) and with -O0 (no optimization,
# the runtime library). One with async code runs a third time on threads,
# each piece that may run ahead of its turn on one of them (run/NAME/threads,
# docs/async.md, section 8). Every tests/errors/*.ts must fail to compile.
# See check.cmake for the expectations.
#
# A program of several files is a directory: tests/run/NAME/main.ts is its
# entry, with the expected output in main.out next to it, and
# tests/errors/NAME/main.ts is compiled with the files it imports, any of
# which may expect errors.
#
# Run tests marked with a `// same-as-node` line behave exactly as in
# JavaScript; when Node.js (23.6 or newer, which strips types) is installed,
# they also run under node against the same expected output. Options for
# node may follow, as in `// same-as-node --experimental-transform-types`
# for enums, which node does not strip. Node needs the extension in an
# import: `from "./math.ts"`.

find_program(TZ_NODE node)

# The name of a test: its file, or the directory of a main.ts.
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

file(GLOB TZ_RUN_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/run/*.ts
     ${CMAKE_CURRENT_SOURCE_DIR}/run/*/main.ts)
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
      COMMAND ${CMAKE_COMMAND} -DNODE=${TZ_NODE} -DSRC=${src} -DMODE=node
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
  set(src ${dir}/${stem}.ts)
  tz_test_name(${src} name multi)
  add_test(NAME explain/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=explain
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
endforeach()

# Examples have no expected output; they only have to compile and exit with 0.
file(GLOB TZ_EXAMPLES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/examples/*.ts)
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

file(GLOB TZ_ERROR_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/errors/*.ts
     ${CMAKE_CURRENT_SOURCE_DIR}/errors/*/main.ts)
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
file(GLOB TZ_CLI_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cmake)
foreach(src ${TZ_CLI_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME cli/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DGIT=${TZ_GIT} -DNODE=${TZ_NODE}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cli -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli/${name}
            -P ${src})
  set_tests_properties(cli/${name} PROPERTIES SKIP_REGULAR_EXPRESSION "skipped: ")
endforeach()
