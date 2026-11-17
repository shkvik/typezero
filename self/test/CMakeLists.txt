# Tests of the compiler written in tz (self/). tz builds it once (self/build),
# and the other tests run that tzparse:
#
#   errors/CODE.tz is a source with the mistake of that error code and must
#     make it print exactly CODE.err; self/codes keeps a test for every code;
#   recovery/NAME.tz has more than the mistake itself and must print exactly
#     NAME.err too;
#   trees/NAME.tz is correct code that must give exactly the tree NAME.tree.
#
# A test of a declaration file is NAME.d.tz next to its module NAME.tz: the
# test is registered by the module, so the declaration files leave the lists
# here. See check.cmake.
set(TZ_SELF ${CMAKE_CURRENT_BINARY_DIR}/tzparse${CMAKE_EXECUTABLE_SUFFIX})
set(TZ_SELF_CHECK ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
add_test(NAME self/build
  COMMAND ${CMAKE_COMMAND} -DTZ=$<TARGET_FILE:tz> -DROOT=${PROJECT_SOURCE_DIR} -DEXE=${TZ_SELF}
          -DMODE=build -P ${TZ_SELF_CHECK})
set_tests_properties(self/build PROPERTIES FIXTURES_SETUP tz_self)
add_test(NAME self/codes
  COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DMODE=codes -P ${TZ_SELF_CHECK})

# Registers a test for every NAME.tz of the directory DIR, run by check.cmake
# in the mode MODE.
function(tz_self_tests dir mode)
  file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.tz)
  list(FILTER sources EXCLUDE REGEX "\\.d\\.tz$")
  foreach(src ${sources})
    get_filename_component(name ${src} NAME_WE)
    add_test(NAME self/${dir}/${name}
      COMMAND ${CMAKE_COMMAND} -DEXE=${TZ_SELF} -DSRC=${src} -DMODE=${mode}
              -P ${TZ_SELF_CHECK})
    set_tests_properties(self/${dir}/${name} PROPERTIES FIXTURES_REQUIRED tz_self)
  endforeach()
endfunction()
tz_self_tests(errors error)
tz_self_tests(recovery recovery)
tz_self_tests(trees tree)

# The enumerations of the language, *.py here: a script builds programs
# together with what the parser must answer (the tree, or the one error),
# runs the tzparse of self/build over them and exits with 1 when an answer
# differs; the head of every script says what it builds. Its report is left
# in the directory of the test. They take minutes, so they have the label
# 'enumeration': `ctest -LE enumeration` runs the tests without them.
find_program(TZ_PYTHON NAMES python python3)
function(tz_self_enumeration name script)
  add_test(NAME self/test/${name}
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${TZ_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/${script}
            ${TZ_SELF} ${CMAKE_CURRENT_BINARY_DIR}/${name} ${ARGN})
  set_tests_properties(self/test/${name} PROPERTIES
    FIXTURES_REQUIRED tz_self LABELS enumeration TIMEOUT 3000)
endfunction()
if(TZ_PYTHON)
  tz_self_enumeration(exprgen exprgen.py all)
  tz_self_enumeration(exprgen_bare exprgen.py all bare)
  tz_self_enumeration(stmtgen stmtgen.py all)
  tz_self_enumeration(typegen typegen.py all)
  tz_self_enumeration(declgen declgen.py)
  tz_self_enumeration(dfilegen dfilegen.py)
  tz_self_enumeration(wronggen wronggen.py)
  tz_self_enumeration(mutgen mutgen.py)
endif()
