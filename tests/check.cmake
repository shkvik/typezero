# Runs one test. Expectations live in the test source as comments:
#
#   run tests (MODE=run):
#     <name>.out next to the source holds the exact expected stdout
#     // expect-exit: N        expected exit code (default 0)
#     // expect-stderr: text   text that must appear in stderr
#     // expect-warning: text  the compiler must warn with text on the next
#                              line that is not such a comment, as for
#                              expect-error below; a warning that no such
#                              comment expects fails the test
#     Programs run with TZ_CHECK_LEAKS set: when main returns, every
#     reference-counted object must have been freed.
#   How the program runs, from lines that start with:
#     // args: a "b c"         its command-line arguments
#     // env: NAME=value       an environment variable; one per line; @TESTS@
#                              in the value is this directory (tests/tls/ca.pem
#                              is @TESTS@/tls/ca.pem)
#     // merge-stderr          stderr goes into stdout, as with `2>&1`, so
#                              <name>.out holds both in the order written
#   and <name>.in next to the source, if there is one: its bytes are stdin,
#   exactly (a last line without "\n", "\r\n", any language), for readLine
#   and readFileSync(0). Without it stdin is what ctest has.
#
#   node tests (MODE=node): the same source run by Node.js the same way must
#     print exactly <name>.out too, and exit with the same code and stderr,
#     which keeps the expectations honest about JavaScript.
#     Options for node may follow the `// same-as-node` marker.
#     node/preload.cjs runs first: it gives Node what tz has built in, the
#     global SystemError and the module "tz:io" (docs/io.md).
#
#   explain tests (MODE=explain): `tz build --explain-parallel` of the source
#     must succeed and print exactly <name>.explain, next to it.
#
#   error tests (MODE=error):
#     // expect-error: text    the compiler must report an error containing
#                              text on the next line that is not such a
#                              comment; several comments may stack up
#     // expect-warning: text  the same for a warning; other warnings are
#                              allowed here
#   With MULTI set, SRC is the main.ts of a program of several files, and the
#   comments may be in any .ts file of its directory.

file(STRINGS ${SRC} directives REGEX "// expect-")
set(expected_exit 0)
set(expected_stderr "")
foreach(line IN LISTS directives)
  if(line MATCHES "// expect-exit: *([0-9]+)")
    set(expected_exit ${CMAKE_MATCH_1})
  elseif(line MATCHES "// expect-stderr: *(.*)$")
    list(APPEND expected_stderr "${CMAKE_MATCH_1}")
  endif()
endforeach()

file(STRINGS ${SRC} run_options REGEX "^// (args:|env:|merge-stderr)" ENCODING UTF-8)
set(program_args "")
set(program_env "")
# THREADS: the parallel runtime (docs/async.md, section 8) with threads, and
# every piece that may run ahead of its turn does (TZ_PARALLEL_STRESS).
if(THREADS)
  list(APPEND program_env TZ_THREADS=8 TZ_PARALLEL_STRESS=1)
endif()
set(stderr_var stderr)
foreach(line IN LISTS run_options)
  if(line MATCHES "^// args: *(.*)$")
    separate_arguments(line_args UNIX_COMMAND "${CMAKE_MATCH_1}")
    list(APPEND program_args ${line_args})
  elseif(line MATCHES "^// env: *(.+)$")
    string(REPLACE "@TESTS@" "${CMAKE_CURRENT_LIST_DIR}" value "${CMAKE_MATCH_1}")
    list(APPEND program_env "${value}")
  elseif(line MATCHES "^// merge-stderr")
    set(stderr_var stdout) # execute_process merges both pipes into the variable
  endif()
endforeach()

function(fail message)
  message(FATAL_ERROR "${message}")
endfunction()

# stdin from <name>.in.
get_filename_component(source_dir ${SRC} DIRECTORY)
get_filename_component(source_name ${SRC} NAME_WE)
set(stdin_option "")
if(EXISTS ${source_dir}/${source_name}.in)
  set(stdin_option INPUT_FILE ${source_dir}/${source_name}.in)
endif()

function(check_exit_and_stderr exit_code stdout stderr)
  if(NOT exit_code EQUAL expected_exit)
    fail("exit code ${exit_code}, expected ${expected_exit}\nstdout:\n${stdout}\nstderr:\n${stderr}")
  endif()
  foreach(text IN LISTS expected_stderr)
    string(FIND "${stderr}" "${text}" found)
    if(found EQUAL -1)
      fail("stderr does not contain '${text}':\n${stderr}")
    endif()
  endforeach()
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

# The output of the compiler as a list of lines. A semicolon would split a
# list element and a bracket would keep it from splitting, so they are
# swapped for placeholders first; paths get '/'.
function(output_lines text out)
  string(REPLACE ";" "<semicolon>" text "${text}")
  string(REPLACE "[" "<open>" text "${text}")
  string(REPLACE "]" "<close>" text "${text}")
  string(REPLACE "\r\n" "\n" text "${text}")
  string(REPLACE "\\" "/" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(${out} "${lines}" PARENT_SCOPE)
endfunction()

# The comments `// expect-<kind>: text` of the test, each as "AT|LINE|text":
# AT is how a message names the line the comment is about, the next one that
# is no such comment ("<file>:<line>:"), and LINE how a failure names it. The
# files with expectations, and how a message names each: by its path in a
# program of several files.
function(collect_expectations kind out)
  get_filename_component(test_dir ${SRC} DIRECTORY)
  if(MULTI)
    file(GLOB_RECURSE sources RELATIVE ${test_dir} ${test_dir}/*.ts)
    list(SORT sources)
  else()
    get_filename_component(sources ${SRC} NAME)
  endif()
  set(result "")
  foreach(rel IN LISTS sources)
    if(MULTI)
      set(where "${test_dir}/${rel}:")
    else()
      set(where ".ts:")
    endif()
    string(REPLACE "\\" "/" where "${where}")
    file(READ ${test_dir}/${rel} source)
    string(REPLACE ";" "<semicolon>" source "${source}")
    string(REPLACE "[" "<open>" source "${source}")
    string(REPLACE "]" "<close>" source "${source}")
    string(REPLACE "\r\n" "\n" source "${source}")
    string(REPLACE "\n" ";" source_lines "${source}")
    set(pending "")
    set(lineno 0)
    foreach(line IN LISTS source_lines)
      math(EXPR lineno "${lineno} + 1")
      if(line MATCHES "^[ \t]*// expect-(error|warning): *(.*)$")
        if(CMAKE_MATCH_1 STREQUAL kind)
          list(APPEND pending "${CMAKE_MATCH_2}")
        endif()
        continue()
      endif()
      foreach(text IN LISTS pending)
        string(REPLACE "\\" "/" text "${text}")
        list(APPEND result "${where}${lineno}:|line ${lineno} of ${rel}|${text}")
      endforeach()
      set(pending "")
    endforeach()
  endforeach()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

# The parts of an expectation of collect_expectations.
function(split_expectation expectation at_var line_var text_var)
  string(FIND "${expectation}" "|" bar)
  string(SUBSTRING "${expectation}" 0 ${bar} at)
  math(EXPR start "${bar} + 1")
  string(SUBSTRING "${expectation}" ${start} -1 rest)
  string(FIND "${rest}" "|" bar)
  string(SUBSTRING "${rest}" 0 ${bar} line)
  math(EXPR start "${bar} + 1")
  string(SUBSTRING "${rest}" ${start} -1 text)
  set(${at_var} "${at}" PARENT_SCOPE)
  set(${line_var} "${line}" PARENT_SCOPE)
  set(${text_var} "${text}" PARENT_SCOPE)
endfunction()

# Whether the message `reported` is the one `expectation` wants.
function(matches reported expectation kind out)
  split_expectation("${expectation}" at line text)
  string(FIND "${reported}" "${at}" at_line)
  string(FIND "${reported}" ": ${kind}: " is_kind)
  string(FIND "${reported}" "${text}" has_text)
  if(NOT at_line EQUAL -1 AND NOT is_kind EQUAL -1 AND NOT has_text EQUAL -1)
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Every expectation of `kind` ("error" or "warning") must be among the
# messages in `output`; with `strict`, every message of that kind must be
# expected too.
function(check_messages kind expectations output strict)
  output_lines("${output}" lines)
  foreach(expectation IN LISTS expectations)
    set(found FALSE)
    foreach(reported IN LISTS lines)
      matches("${reported}" "${expectation}" ${kind} found)
      if(found)
        break()
      endif()
    endforeach()
    if(NOT found)
      split_expectation("${expectation}" at line text)
      foreach(var text output)
        string(REPLACE "<semicolon>" ";" ${var} "${${var}}")
        string(REPLACE "<open>" "[" ${var} "${${var}}")
        string(REPLACE "<close>" "]" ${var} "${${var}}")
      endforeach()
      fail("no ${kind} containing '${text}' on ${line}:\n${output}")
    endif()
  endforeach()
  if(NOT strict)
    return()
  endif()
  foreach(reported IN LISTS lines)
    string(FIND "${reported}" ": ${kind}: " is_kind)
    if(is_kind EQUAL -1)
      continue()
    endif()
    set(found FALSE)
    foreach(expectation IN LISTS expectations)
      matches("${reported}" "${expectation}" ${kind} found)
      if(found)
        break()
      endif()
    endforeach()
    if(NOT found)
      string(REPLACE "<semicolon>" ";" reported "${reported}")
      string(REPLACE "<open>" "[" reported "${reported}")
      string(REPLACE "<close>" "]" reported "${reported}")
      fail("unexpected ${kind}; expect it with '// expect-${kind}:' on the line before:\n${reported}")
    endif()
  endforeach()
endfunction()

if(MODE STREQUAL "run")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env TZ_CHECK_LEAKS=1 ${program_env} ${TZ} run ${OPT} ${SRC}
            ${program_args}
    ${stdin_option}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE ${stderr_var}
    RESULT_VARIABLE exit_code)

  # With merge-stderr a report of leaks lands in stdout, which then differs.
  if(stderr MATCHES "leaked")
    fail("memory leak:\n${stderr}")
  endif()
  check_exit_and_stderr("${exit_code}" "${stdout}" "${stderr}")
  check_stdout("${stdout}")
  collect_expectations(warning expected_warnings)
  check_messages(warning "${expected_warnings}" "${stderr}" TRUE)

elseif(MODE STREQUAL "node")
  # Options for node may follow `// same-as-node`.
  file(STRINGS ${SRC} marker REGEX "^// same-as-node")
  string(REGEX REPLACE "^// same-as-node *" "" node_options "${marker}")
  separate_arguments(node_options)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env ${program_env} ${NODE} --no-warnings
            --require ${CMAKE_CURRENT_LIST_DIR}/node/preload.cjs ${node_options} ${SRC}
            ${program_args}
    ${stdin_option}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE ${stderr_var}
    RESULT_VARIABLE exit_code)
  check_exit_and_stderr("${exit_code}" "${stdout}" "${stderr}")
  check_stdout("${stdout}")

elseif(MODE STREQUAL "explain")
  execute_process(
    COMMAND ${TZ} build --explain-parallel ${SRC}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)
  if(NOT exit_code EQUAL 0)
    fail("tz build --explain-parallel exited with ${exit_code}:\n${stderr}")
  endif()
  get_filename_component(dir ${SRC} DIRECTORY)
  get_filename_component(name ${SRC} NAME_WE)
  file(READ ${dir}/${name}.explain expected)
  string(REPLACE "\r\n" "\n" expected "${expected}")
  string(REPLACE "\r\n" "\n" stdout "${stdout}")
  if(NOT stdout STREQUAL expected)
    fail("the explanation differs from ${name}.explain\n--- expected:\n${expected}\n--- actual:\n${stdout}")
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
  collect_expectations(error expected_errors)
  list(LENGTH expected_errors checked)
  if(checked EQUAL 0)
    fail("the test has no '// expect-error:' lines")
  endif()
  check_messages(error "${expected_errors}" "${stderr}" FALSE)
  collect_expectations(warning expected_warnings)
  check_messages(warning "${expected_warnings}" "${stderr}" FALSE)

else()
  fail("unknown MODE '${MODE}'")
endif()
