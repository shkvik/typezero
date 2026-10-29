# Signals of the system (docs/process.md) and output that is not lost
# (docs/io.md): a server listens to Ctrl+Break with process.once, stops and
# exits with 0; what it printed is in the file it writes to while it still
# runs, not only when it exits; a program without a listener dies of
# Ctrl+Break as before, but keeps what it printed; a listener does not keep a
# program alive. Python sends Ctrl+Break, as Node.js cannot.
include(${SOURCE}/../cli.cmake)
if(NOT PYTHON)
  skip("Python is not installed")
endif()

write(server.tz [[
import { Server } from "tz:http"

const server = new Server({ port: 0, hostname: "127.0.0.1" }, () => new Response("hi"))
const started = server.listen()
if (started.isErr()) {
  console.log(started.error.message)
  process.exit(1)
}
const listening = started.value
const never = () => console.log("never")
process.on("SIGINT", never)
process.off("SIGINT", never)
process.once("SIGBREAK", async () => {
  console.log("got SIGBREAK")
  await listening.stop()
  console.log("stopped")
  process.exit(0)
})
console.log("listening")
]])
write(idle.tz [[
console.log("one")
process.stdout.write("two")
setTimeout(() => console.log("late"), 30000)
]])
write(unref.tz [[
let calls = 0
const count = () => {
  calls++
}
process.on("SIGINT", count)
process.on("SIGTERM", async () => {
  await new Promise<void>(resolve => setTimeout(resolve, 1))
})
console.log("done", calls)
]])
write(driver.py [[
import os, signal, subprocess, sys, time

exe, log, wanted = sys.argv[1], sys.argv[2], sys.argv[3]
with open(log, "wb") as out:
    p = subprocess.Popen([exe], stdout=out, stderr=subprocess.STDOUT,
                         creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    seen = b""
    for _ in range(100):
        time.sleep(0.05)
        with open(log, "rb") as f:
            seen = f.read()
        if seen.endswith(wanted.encode()):
            break
    print("while running:", seen.decode().replace("\n", "|"))
    try:
        os.kill(p.pid, signal.CTRL_BREAK_EVENT)
    except OSError:
        p.kill()
        print("skipped: no console to send Ctrl+Break in")
        sys.exit(0)
    try:
        code = p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
        code = "a timeout"
with open(log, "rb") as f:
    print("at the end:", f.read().decode().replace("\n", "|"))
print("exit code:", code)
]])

tz(build -o server.exe server.tz)
tz(build -o idle.exe idle.tz)
tz(run unref.tz STDOUT "done 0\n")

function(drive exe wanted expected)
  execute_process(COMMAND ${PYTHON} -I ${WORK}/driver.py ${WORK}/${exe} ${WORK}/${exe}.log
                          ${wanted}
    WORKING_DIRECTORY ${WORK}
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
  normalize(out "${out}")
  if(out MATCHES "skipped: ")
    message("${out}")
    return()
  endif()
  if(NOT code EQUAL 0 OR NOT out STREQUAL expected)
    message(FATAL_ERROR "${exe}: exit code ${code}\n--- expected:\n${expected}\n--- actual:\n"
                        "${out}\nstderr:\n${err}")
  endif()
endfunction()

drive(server.exe "listening\n" [[
while running: listening|
at the end: listening|got SIGBREAK|stopped|
exit code: 0
]])
drive(idle.exe "two" [[
while running: one|two
at the end: one|two
exit code: 3221225786
]])
