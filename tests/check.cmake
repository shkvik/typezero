# Runs one test. Expectations live in the test source as comments:
#
#   run tests (MODE=run):
#     <name>.out next to the source holds the exact expected stdout
#     // expect-exit: N        expected exit code (default 0)
#     // expect-stderr: text   text that must appear in stderr
#     Programs run with TZ_CHECK_LEAKS set: when main returns, every
#     reference-counted object must have been freed.
#
#   node tests (MODE=node): the same source run by Node.js must print exactly
#     <name>.out too, which keeps the expectations honest about JavaScript.
#
#   error tests (MODE=error):
#     // expect-error: text    the compiler must report an error containing
#                              text on the next line that is not such a
#                              comment; several comments may stack up

file(STRINGS ${SRC} directives REGEX "// expect-")

function(fail message)
  message(FATAL_ERROR "${message}")
endfunction()

function(check_stdout stdout)
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
endfunction()

if(MODE STREQUAL "run")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env TZ_CHECK_LEAKS=1 ${TZ} run ${OPT} ${SRC}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)

  if(stderr MATCHES "leaked")
    fail("memory leak:\n${stderr}")
  endif()

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
  check_stdout("${stdout}")

elseif(MODE STREQUAL "node")
  execute_process(
    COMMAND ${NODE} --no-warnings ${SRC}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)
  if(NOT exit_code EQUAL 0)
    fail("node exited with ${exit_code}:\n${stderr}")
  endif()
  check_stdout("${stdout}")

elseif(MODE STREQUAL "error")
  execute_process(
    COMMAND ${TZ} build -o ${CMAKE_CURRENT_BINARY_DIR}/error-test.exe ${SRC}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)

  if(exit_code EQUAL 0)
    fail("compiled without errors, but errors were expected")
  endif()
  # The source and the output as lists of lines. A semicolon would split a
  # list element, so it is swapped for a placeholder first.
  file(READ ${SRC} source)
  foreach(var source stderr)
    string(REPLACE ";" "<semicolon>" ${var} "${${var}}")
    string(REPLACE "\r\n" "\n" ${var} "${${var}}")
    string(REPLACE "\n" ";" ${var}_lines "${${var}}")
  endforeach()

  set(checked 0)
  set(pending "")
  set(lineno 0)
  foreach(line IN LISTS source_lines)
    math(EXPR lineno "${lineno} + 1")
    if(line MATCHES "^[ \t]*// expect-error: *(.*)$")
      list(APPEND pending "${CMAKE_MATCH_1}")
      continue()
    endif()
    foreach(text IN LISTS pending)
      math(EXPR checked "${checked} + 1")
      set(found FALSE)
      foreach(reported IN LISTS stderr_lines)
        string(FIND "${reported}" ".ts:${lineno}:" at_line)
        string(FIND "${reported}" "error: " is_error)
        string(FIND "${reported}" "${text}" has_text)
        if(NOT at_line EQUAL -1 AND NOT is_error EQUAL -1 AND NOT has_text EQUAL -1)
          set(found TRUE)
          break()
        endif()
      endforeach()
      if(NOT found)
        string(REPLACE "<semicolon>" ";" text "${text}")
        string(REPLACE "<semicolon>" ";" stderr "${stderr}")
        fail("no error containing '${text}' on line ${lineno}:\n${stderr}")
      endif()
    endforeach()
    set(pending "")
  endforeach()
  if(checked EQUAL 0)
    fail("the test has no '// expect-error:' lines")
  endif()

else()
  fail("unknown MODE '${MODE}'")
endif()
