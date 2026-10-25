# tz build of a program whose executable is running: Windows lets nothing
# replace it, and tz says so.
include(${SOURCE}/../cli.cmake)

if(NOT CMAKE_HOST_WIN32)
  skip("only Windows locks the executable of a running program")
endif()

write(wait.ts [[
import { readLine } from "tz:io"
// Runs until its input ends.
while (readLine() != undefined) {}
]])
tz(build wait.ts)

# wait.exe starts right after tz, long before tz links, and reads what tz
# prints: it runs until tz ends.
execute_process(COMMAND ${TZ} build wait.ts COMMAND ${WORK}/wait.exe
  WORKING_DIRECTORY ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
normalize(err "${err}")
if(NOT codes STREQUAL "1;0")
  message(FATAL_ERROR "tz build of a running program: exit codes ${codes}, expected 1;0\n"
                      "stderr:\n${err}")
endif()
string(CONCAT expected "tz: cannot write 'wait.exe': it is in use, probably still running; "
                       "stop it and build again\n")
if(NOT err STREQUAL expected)
  message(FATAL_ERROR "tz build of a running program: stderr differs\n--- expected:\n"
                      "${expected}\n--- actual:\n${err}")
endif()
file(GLOB left ${WORK}/*.tmp*)
if(left)
  message(FATAL_ERROR "tz build left files behind: ${left}")
endif()

# Once the program has ended, the build goes through.
tz(build wait.ts)
