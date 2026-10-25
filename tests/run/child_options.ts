// same-as-node
// Wrong options are the TypeErrors and RangeErrors of Node, with its texts;
// what goes wrong when starting the program is the error of spawnSync.
import { spawnSync, execSync, execFileSync } from "node:child_process"

function show(run: () => void) {
  try {
    run()
    console.log("no error")
  } catch (e) {
    console.log(e.name, "|", e.message)
  }
}
show(() => spawnSync("a\u0000b"))
show(() => spawnSync("cmd", ["/c", "a\u0000"]))
show(() => spawnSync(""))
show(() => execSync(""))
show(() => execSync("echo \u0000"))
show(() => execFileSync("cmd", { cwd: "a\u0000" }))
show(() => spawnSync("cmd", ["/c", "exit"], { env: { A: "a\u0000" } }))
show(() => spawnSync("cmd", ["/c", "exit"], { argv0: "a\u0000" }))
show(() => spawnSync("cmd", ["/c", "exit"], { shell: "a\u0000" }))
show(() => spawnSync("cmd", ["/c", "exit"], { timeout: -1 }))
show(() => spawnSync("cmd", ["/c", "exit"], { timeout: 1.5 }))
show(() => spawnSync("cmd", ["/c", "exit"], { maxBuffer: -1 }))
show(() => spawnSync("cmd", ["/c", "exit"], { killSignal: "SIGFOO" }))
show(() => spawnSync("cmd", ["/c", "exit"], { killSignal: 99 }))
const signal = "SIGKILL"
show(() => console.log(spawnSync("cmd", ["/c", "exit 5"], { killSignal: signal, timeout: 10000 }).status))

// PATH is looked for in the environment the program gets, its name in any
// case; without it, this process's.
console.log(spawnSync("cmd", ["/c", "echo found"], { env: { OTHER: "1" }, encoding: "utf8" }).stdout.trim())
console.log(spawnSync("cmd", ["/c", "echo found"], { env: { path: "" }, encoding: "utf8" }).error?.code)
console.log(spawnSync("cmd.exe", ["/c", "exit 9"]).status)
console.log(spawnSync("CMD.EXE", ["/c", "exit 0"], { windowsHide: true, detached: true }).status)
// A working directory that is not there; an empty shell is none, as in Node.
console.log(spawnSync("cmd", ["/c", "echo x"], { cwd: "C:\\tz-no-such-dir" }).error?.message)
show(() => execSync("echo hi", { shell: "" }))
console.log(execFileSync("cmd", ["/c", "echo", "no shell"], { shell: "", encoding: "utf8" }).trim())
const notStarted = spawnSync("cmd", ["/c", "exit 1"], { uid: 0 })
console.log(notStarted.error?.code, notStarted.error?.message)
