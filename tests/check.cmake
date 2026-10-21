# Runs one test. Expectations live in the test source as comments:
#
#   run tests (MODE=run):
#     <name>.out next to the source holds the exact expected stdout
#     // expect-exit: N        expected exit code (default 0)
#     // expect-stderr: text   text that must appear in stderr
#
#   error tests (MODE=error):
#     // expect-error: text    text that must appear in the compiler's output;
#                              the line number of the comment is not checked

file(STRINGS ${SRC} directives REGEX "// expect-")

function(fail message)
  message(FATAL_ERROR "${message}")
endfunction()

if(MODE STREQUAL "run")
  execute_process(
    COMMAND ${TZ} run ${OPT} ${SRC}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)

  set(expected_exit 0)
  set(expected_stderr "")
  foreach(line IN LISTS directives)
    if(line MATCHES "// expect-exit: *([0-9]+)")
      set(expected_exit ${CMAKE_MATCH_1})
    elseif(line MATCHES "// expect-stderr: *(.*)$")
      list(APPEND expected_stderr "${CMAKE_MATCH_1}")
    endif()
  endforeach()

  if(NOT exit_code EQUAL expected_exit)
    fail("exit code ${exit_code}, expected ${expected_exit}\nstdout:\n${stdout}\nstderr:\n${stderr}")
  endif()
  foreach(text IN LISTS expected_stderr)
    string(FIND "${stderr}" "${text}" found)
    if(found EQUAL -1)
      fail("stderr does not contain '${text}':\n${stderr}")
    endif()
  endforeach()

  get_filename_component(dir ${SRC} DIRECTORY)
  get_filename_component(name ${SRC} NAME_WE)
  set(expected_file ${dir}/${name}.out)
  if(EXISTS ${expected_file})
    file(READ ${expected_file} expected)
    string(REPLACE "\r\n" "\n" expected "${expected}")
    string(REPLACE "\r\n" "\n" stdout "${stdout}")
    if(NOT stdout STREQUAL expected)
      fail("stdout differs from ${name}.out\n--- expected:\n${expected}\n--- actual:\n${stdout}")
    endif()
  endif()

elseif(MODE STREQUAL "error")
  execute_process(
    COMMAND ${TZ} build -o ${CMAKE_CURRENT_BINARY_DIR}/error-test.exe ${SRC}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)

  if(exit_code EQUAL 0)
    fail("compiled without errors, but errors were expected")
  endif()
  set(checked 0)
  foreach(line IN LISTS directives)
    if(line MATCHES "// expect-error: *(.*)$")
      math(EXPR checked "${checked} + 1")
      string(FIND "${stderr}" "${CMAKE_MATCH_1}" found)
      if(found EQUAL -1)
        fail("the output does not contain '${CMAKE_MATCH_1}':\n${stderr}")
      endif()
    endif()
  endforeach()
  if(checked EQUAL 0)
    fail("the test has no '// expect-error:' lines")
  endif()

else()
  fail("unknown MODE '${MODE}'")
endif()
