// same-as-node
// readFileSync(0, "utf8") reads all of stdin (io_stdin.in) at once, as it
// is: "\r\n" stays.
import { readFileSync } from "node:fs"

const all = readFileSync(0, "utf8")
console.log(JSON.stringify(all), all.length)
const lines = all.split("\n").map(l => l.trimEnd())
console.log(lines, lines.length)
console.log(JSON.stringify(readFileSync(0, "utf8")))
