// same-as-node
// expect-exit: 1
// expect-stderr: Error: ENOENT: no such file or directory, open '
// expect-stderr: code: 'ENOENT',
// expect-stderr: syscall: 'open',
// A SystemError nobody catches prints as Node prints it: the text, the
// stack, then its fields.
import { readFileSync } from "node:fs"

console.log("before")
readFileSync("tz-no-such-file.txt", "utf8")
console.log("after")
