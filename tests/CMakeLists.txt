# Every tests/run/*.ts is compiled and run twice: as usual (optimized, with
# the runtime bitcode linked into the program) and with -O0 (no optimization,
# the runtime library). Every tests/errors/*.ts must fail to compile. See
# check.cmake for the expectations.
#
# Run tests marked with a `// same-as-node` line behave exactly as in
# JavaScript; when Node.js (23.6 or newer, which strips types) is installed,
# they also run under node against the same expected output. Options for
# node may follow, as in `// same-as-node --experimental-transform-types`
# for enums, which node does not strip.

find_program(TZ_NODE node)

file(GLOB TZ_RUN_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/run/*.ts)
foreach(src ${TZ_RUN_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME run/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=run
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
  add_test(NAME run/${name}/O0
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=run -DOPT=-O0
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)

  file(STRINGS ${src} same_as_node REGEX "^// same-as-node")
  if(TZ_NODE AND same_as_node)
    add_test(NAME node/${name}
      COMMAND ${CMAKE_COMMAND} -DNODE=${TZ_NODE} -DSRC=${src} -DMODE=node
              -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
  endif()
endforeach()

# Examples have no expected output; they only have to compile and exit with 0.
file(GLOB TZ_EXAMPLES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/examples/*.ts)
foreach(src ${TZ_EXAMPLES})
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME examples/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=run
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
endforeach()

file(GLOB TZ_ERROR_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/errors/*.ts)
foreach(src ${TZ_ERROR_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME errors/${name}
    COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DSRC=${src} -DMODE=error
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
endforeach()
