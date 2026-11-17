# Tests of the compiler written in tz (self/).
#
#   MODE=build: `tz build` of self/main.tz into EXE must succeed. The other
#     modes run that EXE.
#
#   MODE=error: tests/self/errors/CODE.tz is a source with a mistake, named
#     after the code of the error it is about (TZ1221.tz). `EXE check CODE.tz`,
#     run in the directory of SRC, must exit with 1 and print to stderr
#     exactly CODE.err, and one of the errors must have that code. With
#     ACCEPT=ON what it prints is written to CODE.err instead: after a change
#     of messages, read the difference before committing it.
#
#   MODE=codes: every code of self/diagnostic/code.tz has its test in
#     tests/self/errors, and every test there is named after a code.
#     UNREACHABLE lists the codes no source can get: the parser asks for them
#     where an earlier look ahead has already seen the token.
cmake_minimum_required(VERSION 3.20)

# TZ1508: '(' of the parameters of an arrow function; TZ1511: '=>' after them.
# An arrow function is parsed only after a scan found both.
set(UNREACHABLE TZ1508 TZ1511)

function(fail message)
  message(FATAL_ERROR "${message}")
endfunction()

if(MODE STREQUAL "build")
  get_filename_component(dir ${EXE} DIRECTORY)
  file(MAKE_DIRECTORY ${dir})
  execute_process(
    COMMAND ${TZ} build -o ${EXE} ${ROOT}/self/main.tz
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)
  if(NOT exit_code EQUAL 0)
    fail("tz build of self/main.tz exited with ${exit_code}:\n${stdout}${stderr}")
  endif()

elseif(MODE STREQUAL "error")
  get_filename_component(dir ${SRC} DIRECTORY)
  get_filename_component(code ${SRC} NAME_WE)
  execute_process(
    COMMAND ${EXE} check ${code}.tz
    WORKING_DIRECTORY ${dir}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)
  string(REPLACE "\r\n" "\n" stderr "${stderr}")
  if(NOT exit_code EQUAL 1)
    fail("exit code ${exit_code}, expected 1\nstdout:\n${stdout}\nstderr:\n${stderr}")
  endif()
  string(FIND "${stderr}" ": error ${code}: " at)
  if(at EQUAL -1)
    fail("no error ${code}:\n${stderr}")
  endif()
  if(ACCEPT)
    file(WRITE ${dir}/${code}.err "${stderr}")
    return()
  endif()
  if(NOT EXISTS ${dir}/${code}.err)
    fail("there is no ${code}.err with the expected messages; the messages are:\n${stderr}")
  endif()
  file(READ ${dir}/${code}.err expected)
  string(REPLACE "\r\n" "\n" expected "${expected}")
  if(NOT stderr STREQUAL expected)
    fail("the messages differ from ${code}.err\n--- expected:\n${expected}\n--- actual:\n${stderr}")
  endif()

elseif(MODE STREQUAL "codes")
  file(STRINGS ${ROOT}/self/diagnostic/code.tz lines REGEX "= \"TZ[0-9]+\"")
  set(codes "")
  set(problems "")
  foreach(line IN LISTS lines)
    string(REGEX MATCH "TZ[0-9]+" code "${line}")
    list(APPEND codes ${code})
    list(FIND UNREACHABLE ${code} unreachable)
    if(EXISTS ${ROOT}/tests/self/errors/${code}.tz)
      if(NOT unreachable EQUAL -1)
        string(APPEND problems "${code} has a test: take it out of UNREACHABLE in tests/self.cmake\n")
      endif()
    elseif(unreachable EQUAL -1)
      string(APPEND problems "${code} has no test tests/self/errors/${code}.tz\n")
    endif()
  endforeach()
  file(GLOB tests RELATIVE ${ROOT}/tests/self/errors ${ROOT}/tests/self/errors/*)
  foreach(test IN LISTS tests)
    get_filename_component(code ${test} NAME_WE)
    list(FIND codes ${code} known)
    if(known EQUAL -1)
      string(APPEND problems "${test}: there is no code ${code} in self/diagnostic/code.tz\n")
    endif()
  endforeach()
  foreach(code IN LISTS UNREACHABLE)
    list(FIND codes ${code} known)
    if(known EQUAL -1)
      string(APPEND problems "UNREACHABLE of tests/self.cmake names ${code}, which is no code\n")
    endif()
  endforeach()
  if(problems)
    fail("${problems}")
  endif()

else()
  fail("unknown MODE '${MODE}'")
endif()
