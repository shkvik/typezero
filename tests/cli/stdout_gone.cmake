# The errors of process.stdin and process.stdout as streams (docs/streams.md,
# section 3): when the program that reads the output exits, as `head` does,
# a write to process.stdout gives StdoutError (kind STDOUT_ERROR, code EOF in
# Windows and EPIPE elsewhere, as node has it), and the stream stops.
include(${SOURCE}/../cli.cmake)

write(writer.tz [[
import { stdout } from "node:process"

for (let i = 0; i < 5000; i++) {
  stdout.write(`line ${i}\n`, (e) => {
    if (e != undefined) console.error(`${e.kind} ${e.code}: ${e.message}`)
  })
  if (stdout.errored() != undefined) {
    console.error("stopped")
    break
  }
  await new Promise<void>((resolve) => setTimeout(resolve, 1))
}
]])
write(reader.tz [[
import { readLine } from "tz:io"

const first = readLine()
const second = readLine()
console.log(first ?? "", second ?? "")
]])
tz(build writer.tz)
tz(build reader.tz)
if(WIN32)
  set(exe ".exe")
else()
  set(exe "")
endif()
execute_process(COMMAND ${WORK}/writer${exe} COMMAND ${WORK}/reader${exe}
  WORKING_DIRECTORY ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
normalize(out "${out}")
normalize(err "${err}")
if(NOT out STREQUAL "line 0 line 1\n")
  message(FATAL_ERROR "the reader got:\n${out}\nstderr:\n${err}")
endif()
if(NOT err MATCHES "STDOUT_ERROR (EOF|EPIPE): could not write to stdout: the reader is gone")
  message(FATAL_ERROR "no StdoutError in stderr:\n${err}")
endif()
if(NOT err MATCHES "stopped")
  message(FATAL_ERROR "the writer did not stop:\n${err}")
endif()
