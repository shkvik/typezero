# Tests of the compiler written in tz (self/); CMakeLists.txt here registers
# them.
#
#   MODE=build: `tz build` of self/main.tz into EXE must succeed. The other
#     modes run that EXE. The files of library/ are copied into lib/ next to
#     EXE, where the loader looks for the modules of the library.
#
#   MODE=error: self/test/errors/CODE.tz is a source with a mistake, named
#     after the code of the error it is about (TZ1221.tz). `EXE check CODE.tz`,
#     run in the directory of SRC, must exit with 1 and print to stderr
#     exactly CODE.err, and one of the errors must have that code. With
#     ACCEPT=ON what it prints is written to CODE.err instead: after a change
#     of messages, read the difference before committing it.
#
#   MODE=recovery: self/test/recovery/NAME.tz has several mistakes, or one
#     and correct code around it. `EXE check NAME.tz` must print exactly
#     NAME.err: every mistake once, and nothing that only follows from an
#     earlier one. ACCEPT=ON works as for MODE=error.
#
#   MODE=tree: self/test/trees/NAME.tz is correct code whose tree is easy to
#     get wrong. `EXE tree NAME.tz` must report no error and print exactly
#     NAME.tree. ACCEPT=ON works as for MODE=error.
#
#   MODE=program: self/test/programs/NAME/ and self/test/names/NAME/ are
#     programs of several files with the entry main.tz (SRC). `EXE COMMAND
#     main.tz`, run in that directory, must print to stdout exactly main.out.
#     COMMAND=load (programs) prints the modules, a module after the ones it
#     imports, each with where its imports lead; COMMAND=names (names) prints
#     what every module declares, exports and imports, each name with the
#     declaration it leads to. When main.err is there, the program has
#     mistakes: the exit code must be 1 and stderr exactly main.err; without
#     it the exit code is 0 and stderr is empty. A directory named after a
#     code (TZ2007) is the test of that code: one of the errors must have it.
#     ACCEPT=ON writes main.out and main.err.
#
#   MODE=codes: every code of self/diagnostic/code.tz has its test,
#     self/test/errors/CODE.tz or main.tz in self/test/programs/CODE or
#     self/test/names/CODE, and every test in self/test/errors is named after
#     a code. A code no source can get does not stay in code.tz.
#
#   A test of a declaration file: when NAME.d.tz is next to NAME.tz, the file
#     that is checked is NAME.d.tz, and NAME.tz is its module. tzparse reads
#     X.d.tz as a declaration file only when the module X.tz is there, as the
#     compiler does.
cmake_minimum_required(VERSION 3.20)

function(fail message)
  message(FATAL_ERROR "${message}")
endfunction()

function(checked_file dir name result)
  if(EXISTS ${dir}/${name}.d.tz)
    set(${result} ${name}.d.tz PARENT_SCOPE)
  else()
    set(${result} ${name}.tz PARENT_SCOPE)
  endif()
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
  file(GLOB_RECURSE library ${ROOT}/library/*.tz)
  file(REMOVE_RECURSE ${dir}/lib)
  file(COPY ${library} DESTINATION ${dir}/lib)

elseif(MODE STREQUAL "error" OR MODE STREQUAL "recovery")
  get_filename_component(dir ${SRC} DIRECTORY)
  get_filename_component(code ${SRC} NAME_WE)
  checked_file(${dir} ${code} file)
  execute_process(
    COMMAND ${EXE} check ${file}
    WORKING_DIRECTORY ${dir}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)
  string(REPLACE "\r\n" "\n" stderr "${stderr}")
  if(NOT exit_code EQUAL 1)
    fail("exit code ${exit_code}, expected 1\nstdout:\n${stdout}\nstderr:\n${stderr}")
  endif()
  string(FIND "${stderr}" ": error ${code}: " at)
  if(MODE STREQUAL "error" AND at EQUAL -1)
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

elseif(MODE STREQUAL "tree")
  get_filename_component(dir ${SRC} DIRECTORY)
  get_filename_component(name ${SRC} NAME_WE)
  checked_file(${dir} ${name} file)
  execute_process(
    COMMAND ${EXE} tree ${file}
    WORKING_DIRECTORY ${dir}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)
  string(REPLACE "\r\n" "\n" stdout "${stdout}")
  if(NOT exit_code EQUAL 0)
    fail("exit code ${exit_code}, expected 0\nstdout:\n${stdout}\nstderr:\n${stderr}")
  endif()
  string(FIND "${stdout}" "\n!! " at)
  if(NOT at EQUAL -1)
    fail("the parser reports an error for correct code:\n${stdout}")
  endif()
  if(ACCEPT)
    file(WRITE ${dir}/${name}.tree "${stdout}")
    return()
  endif()
  if(NOT EXISTS ${dir}/${name}.tree)
    fail("there is no ${name}.tree with the expected tree; the tree is:\n${stdout}")
  endif()
  file(READ ${dir}/${name}.tree expected)
  string(REPLACE "\r\n" "\n" expected "${expected}")
  if(NOT stdout STREQUAL expected)
    fail("the tree differs from ${name}.tree\n--- expected:\n${expected}\n--- actual:\n${stdout}")
  endif()

elseif(MODE STREQUAL "program")
  get_filename_component(dir ${SRC} DIRECTORY)
  get_filename_component(name ${dir} NAME)
  execute_process(
    COMMAND ${EXE} ${COMMAND} main.tz
    WORKING_DIRECTORY ${dir}
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    RESULT_VARIABLE exit_code)
  string(REPLACE "\r\n" "\n" stdout "${stdout}")
  string(REPLACE "\r\n" "\n" stderr "${stderr}")
  if(name MATCHES "^TZ[0-9]+$")
    string(FIND "${stderr}" ": error ${name}: " at)
    if(at EQUAL -1)
      fail("no error ${name}:\n${stderr}")
    endif()
  endif()
  if(ACCEPT)
    file(WRITE ${dir}/main.out "${stdout}")
    if(stderr STREQUAL "")
      file(REMOVE ${dir}/main.err)
    else()
      file(WRITE ${dir}/main.err "${stderr}")
    endif()
    return()
  endif()
  if(NOT EXISTS ${dir}/main.out)
    fail("there is no main.out with the expected output; the output is:\n${stdout}")
  endif()
  file(READ ${dir}/main.out expected)
  string(REPLACE "\r\n" "\n" expected "${expected}")
  if(NOT stdout STREQUAL expected)
    fail("the output differs from main.out\n--- expected:\n${expected}\n--- actual:\n${stdout}")
  endif()
  set(expected "")
  set(expected_exit 0)
  if(EXISTS ${dir}/main.err)
    file(READ ${dir}/main.err expected)
    string(REPLACE "\r\n" "\n" expected "${expected}")
    set(expected_exit 1)
  endif()
  if(NOT exit_code EQUAL expected_exit)
    fail("exit code ${exit_code}, expected ${expected_exit}\nstderr:\n${stderr}")
  endif()
  if(NOT stderr STREQUAL expected)
    fail("the messages differ from main.err\n--- expected:\n${expected}\n--- actual:\n${stderr}")
  endif()

elseif(MODE STREQUAL "codes")
  file(STRINGS ${ROOT}/self/diagnostic/code.tz lines REGEX "= \"TZ[0-9]+\"")
  set(codes "")
  set(problems "")
  foreach(line IN LISTS lines)
    string(REGEX MATCH "TZ[0-9]+" code "${line}")
    list(APPEND codes ${code})
    if(NOT EXISTS ${ROOT}/self/test/errors/${code}.tz AND
       NOT EXISTS ${ROOT}/self/test/programs/${code}/main.tz AND
       NOT EXISTS ${ROOT}/self/test/names/${code}/main.tz)
      string(APPEND problems
        "${code} has no test: self/test/errors/${code}.tz, or main.tz in self/test/programs/${code} or self/test/names/${code}\n")
    endif()
  endforeach()
  file(GLOB tests RELATIVE ${ROOT}/self/test/errors ${ROOT}/self/test/errors/*)
  foreach(test IN LISTS tests)
    get_filename_component(code ${test} NAME_WE)
    list(FIND codes ${code} known)
    if(known EQUAL -1)
      string(APPEND problems "${test}: there is no code ${code} in self/diagnostic/code.tz\n")
    endif()
  endforeach()
  if(problems)
    fail("${problems}")
  endif()

else()
  fail("unknown MODE '${MODE}'")
endif()
