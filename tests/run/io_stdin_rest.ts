// What readLine has not taken yet, readFileSync(0) gets: both read the same
// stdin through one buffer (in Node, readline would have read ahead).
import { readFileSync } from "node:fs"
import { readLine } from "tz:io"

console.log(readLine())
console.log(JSON.stringify(readFileSync(0, "utf8")))
console.log(readLine())
