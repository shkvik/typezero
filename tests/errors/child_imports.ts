// What node:child_process has that tz has not yet, or never will.
// expect-error: 'fork' of 'node:child_process' is not possible: it starts another Node
// expect-error: 'spawn' of 'node:child_process' is not supported yet: it comes with EventEmitter and the streams of Node (docs/process.md, step 2); use 'spawnSync'
// expect-error: 'exec' of 'node:child_process' is not supported yet
// expect-error: 'execFile' of 'node:child_process' is not supported yet
// expect-error: 'ChildProcess' of 'node:child_process' is not supported yet
// expect-error: 'node:child_process' has no export named 'run'
import { fork, spawn, exec, execFile, ChildProcess, run } from "node:child_process"
// expect-error: 'import * as' is not supported; import the names this file uses: 'import { execSync } from "child_process"'
import * as cp from "child_process"

cp.execSync("echo")
