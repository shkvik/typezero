// same-as-node
// expect-exit: 1
// expect-stderr: Error: Command failed: exit 3
// expect-stderr: status: 3,
// An ExecSyncError nobody catches prints as Node prints it: the text, the
// stack, then the result of spawnSync in it.
import { execSync } from "node:child_process"

console.log("before")
execSync("exit 3")
console.log("after")
