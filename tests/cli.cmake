# Helpers for the tests of tz's commands: each tests/cli/NAME.cmake includes
# this file and runs tz the way a person would, in directories of its own.
#
# A test runs with TZ (the compiler), GIT, NODE and PYTHON (empty if they are
# not installed), SOURCE (tests/cli) and WORK, a fresh directory for the test.
# The cache of packages is WORK/cache and the JSR registry is WORK/jsr, a
# mirror on the disk that jsr_publish fills: no test touches the machine's
# cache or the network. Programs run with TZ_CHECK_LEAKS set, as in the run
# tests.

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})
set(ENV{TZ_CACHE} ${WORK}/cache)
set(ENV{TZ_JSR_URL} "file:///${WORK}/jsr")
set(ENV{TZ_CHECK_LEAKS} 1)

# Skips the rest of the test (ctest reports it as skipped).
macro(skip reason)
  message("skipped: ${reason}")
  return()
endmacro()

# Text as the expectations write it: "\n" line ends, '/' in paths, and the
# test's directory as <work>.
function(normalize var text)
  string(REPLACE "\r\n" "\n" text "${text}")
  string(REPLACE "\\" "/" text "${text}")
  string(REPLACE "${WORK}" "<work>" text "${text}")
  set(${var} "${text}" PARENT_SCOPE)
endfunction()

# tz(<arguments>... [IN <dir>] [EXIT <code>] [STDOUT <text>] [STDERR <text>]...
#    [SERVE <dir>])
# Runs tz in <dir> (relative to WORK; default WORK). The exit code must be
# <code> (default 0), stdout must be <text> exactly if STDOUT is given, and
# every STDERR text must appear in stderr. With SERVE, the registry is <dir>
# (relative to WORK) served over HTTP by serve.mjs. The output is left in
# TZ_STDOUT and TZ_STDERR.
function(tz)
  cmake_parse_arguments(PARSE_ARGV 0 T "" "IN;EXIT;STDOUT;SERVE" "STDERR")
  set(dir ${WORK})
  if(DEFINED T_IN)
    set(dir ${WORK}/${T_IN})
  endif()
  if(NOT DEFINED T_EXIT)
    set(T_EXIT 0)
  endif()
  set(through "")
  if(DEFINED T_SERVE)
    set(through ${NODE} ${SOURCE}/serve.mjs ${WORK}/${T_SERVE})
  endif()
  execute_process(COMMAND ${through} ${TZ} ${T_UNPARSED_ARGUMENTS}
    WORKING_DIRECTORY ${dir}
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
  normalize(out "${out}")
  normalize(err "${err}")
  set(what "tz ${T_UNPARSED_ARGUMENTS} (in ${dir})")
  if(NOT code EQUAL T_EXIT)
    message(FATAL_ERROR "${what}: exit code ${code}, expected ${T_EXIT}\n"
                        "stdout:\n${out}\nstderr:\n${err}")
  endif()
  if(err MATCHES "leaked")
    message(FATAL_ERROR "${what}: memory leak:\n${err}")
  endif()
  if(DEFINED T_STDOUT AND NOT out STREQUAL T_STDOUT)
    message(FATAL_ERROR "${what}: stdout differs\n--- expected:\n${T_STDOUT}\n--- actual:\n${out}"
                        "\nstderr:\n${err}")
  endif()
  foreach(text IN LISTS T_STDERR)
    string(FIND "${err}" "${text}" found)
    if(found EQUAL -1)
      message(FATAL_ERROR "${what}: stderr does not contain '${text}':\n${err}")
    endif()
  endforeach()
  set(TZ_STDOUT "${out}" PARENT_SCOPE)
  set(TZ_STDERR "${err}" PARENT_SCOPE)
endfunction()

# write(<file> <text>): a file relative to WORK.
function(write file text)
  file(WRITE ${WORK}/${file} "${text}")
endfunction()

# expect_file(<file> <text>): the file (relative to WORK) holds exactly <text>.
function(expect_file file text)
  if(NOT EXISTS ${WORK}/${file})
    message(FATAL_ERROR "${file} does not exist")
  endif()
  file(READ ${WORK}/${file} actual)
  normalize(actual "${actual}")
  if(NOT actual STREQUAL text)
    message(FATAL_ERROR "${file} differs\n--- expected:\n${text}\n--- actual:\n${actual}")
  endif()
endfunction()

# expect_match(<file> <regex>): the file (relative to WORK) matches <regex>.
function(expect_match file regex)
  file(READ ${WORK}/${file} actual)
  normalize(actual "${actual}")
  if(NOT actual MATCHES "${regex}")
    message(FATAL_ERROR "${file} does not match '${regex}':\n${actual}")
  endif()
endfunction()

# git(<arguments>... IN <dir>): runs git in <dir> (relative to WORK), the same
# way on every machine.
function(git)
  cmake_parse_arguments(PARSE_ARGV 0 G "" "IN" "")
  execute_process(COMMAND ${GIT} -c user.name=tz -c user.email=tz@example.com
            -c commit.gpgsign=false -c tag.gpgsign=false -c init.defaultBranch=main
            -c core.autocrlf=false ${G_UNPARSED_ARGUMENTS}
    WORKING_DIRECTORY ${WORK}/${G_IN}
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "git ${G_UNPARSED_ARGUMENTS}: ${code}\n${out}${err}")
  endif()
  string(STRIP "${out}" out)
  set(GIT_OUT "${out}" PARENT_SCOPE)
endfunction()

# jsr_publish(<@scope/name> <version> <dir> [YANKED]): puts the files of
# <dir> (relative to SOURCE) into the mirror WORK/jsr as JSR's API serves a
# version of a package (https://jsr.io/docs/api): <version>/<files>,
# <version>_meta.json with every file's SHA-256 and the exports of the
# package's jsr.json, and the version in meta.json.
function(jsr_publish package version dir)
  cmake_parse_arguments(PARSE_ARGV 3 P "YANKED" "" "")
  set(src ${SOURCE}/${dir})
  set(root ${WORK}/jsr/${package})
  file(GLOB_RECURSE files RELATIVE ${src} ${src}/*)
  list(SORT files)
  set(manifest "{}")
  foreach(f IN LISTS files)
    file(SHA256 ${src}/${f} sum)
    file(SIZE ${src}/${f} size)
    string(JSON manifest SET "${manifest}" "/${f}"
           "{\"size\": ${size}, \"checksum\": \"sha256-${sum}\"}")
    configure_file(${src}/${f} ${root}/${version}/${f} COPYONLY)
  endforeach()
  file(READ ${src}/jsr.json jsr)
  string(JSON kind TYPE "${jsr}" exports)
  string(JSON exports GET "${jsr}" exports)
  if(kind STREQUAL "STRING")
    set(exports "{\".\": \"${exports}\"}")
  endif()
  file(WRITE ${root}/${version}_meta.json
       "{\"manifest\": ${manifest}, \"moduleGraph2\": {}, \"exports\": ${exports}}")
  string(REGEX MATCH "^@([^/]+)/(.+)$" ignored "${package}")
  set(meta "{\"scope\": \"${CMAKE_MATCH_1}\", \"name\": \"${CMAKE_MATCH_2}\", \"versions\": {}}")
  if(EXISTS ${root}/meta.json)
    file(READ ${root}/meta.json meta)
  endif()
  set(entry "{}")
  if(P_YANKED)
    set(entry "{\"yanked\": true}")
  endif()
  string(JSON meta SET "${meta}" versions ${version} "${entry}")
  file(WRITE ${root}/meta.json "${meta}")
endfunction()
