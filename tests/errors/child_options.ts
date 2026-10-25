// The options of the functions of node:child_process that decide the type
// of what they give are literals, and the others must have the right type.
import { spawnSync, execSync, execFileSync } from "node:child_process"

const enc = "utf8"
// expect-error: the option 'encoding' of 'spawnSync' is written as a literal, e.g. 'utf8'
spawnSync("git", ["status"], { encoding: enc })
// expect-error: unknown encoding 'utf9'
execSync("git status", { encoding: "utf9" })
const mode = "inherit"
// expect-error: the option 'stdio' of 'execFileSync' is written as a literal
execFileSync("git", { stdio: mode })
// expect-error: 'ipc' is the channel to another Node that fork starts
spawnSync("git", [], { stdio: ["pipe", "ipc"] })
// expect-error: only stdin, stdout and stderr are supported in 'stdio'
spawnSync("git", [], { stdio: ["pipe", "pipe", "pipe", "pipe"] })
// expect-error: unknown stdio 'pipes'
spawnSync("git", [], { stdio: "pipes" })
// expect-error: 'spawnSync' has no option 'stdin'
spawnSync("git", [], { stdin: "x" })
// expect-error: 'execSync' always runs the command through a shell
execSync("git status", { shell: false })
// expect-error: 'input' of 'execSync' is the text or the bytes the program reads
execSync("sort", { input: 42 })
// expect-error: text input with the encoding 'buffer' is a TypeError in Node
execSync("sort", { input: "b\na", encoding: "buffer" })
// expect-error: 'killSignal' is the name of a signal
spawnSync("git", [], { killSignal: true })
// expect-error: 'shell' is true, false or the path of a shell
execFileSync("git", [], { shell: 1 })
const options = { cwd: "." }
// expect-error: the options of 'spawnSync' are written in the call, as an object literal
spawnSync("git", [], options)
// expect-error: the options of 'execFileSync' come after its arguments
execFileSync("git", { cwd: "." }, ["status"])
// expect-error: 'new ExecSyncError' is not possible: only the functions of node:child_process make ExecSyncError
const e = new ExecSyncError()
// expect-error: class 'MyError' cannot extend 'ExecSyncError': only the functions of node:child_process make its objects; extend Error
class MyError extends ExecSyncError {}
// expect-error: type 'string | undefined' is not assignable to type 'string'
const out: string = execSync("dir", { encoding: "utf8", stdio: "inherit" })
