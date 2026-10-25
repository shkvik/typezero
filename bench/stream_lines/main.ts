// A pipeline of streams over a 32 MB file of 800 thousand lines: the file in
// chunks of 64 KiB as text, a Transform that cuts them into lines (object
// mode), one that keeps the lines with "7" and makes them upper case, and a
// Writable that counts them. The file is made once, in the temporary
// directory, so that the runs measure the streams.
import { createReadStream, existsSync, renameSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import { Transform, Writable } from "node:stream"
import { pipeline } from "node:stream/promises"

const path = join(tmpdir(), "tz-bench-stream-lines.txt")
if (!existsSync(path)) {
  const parts: string[] = []
  let seed = 1
  for (let i = 0; i < 800000; i++) {
    seed = (seed * 75 + 74) % 65537
    parts.push(`line ${i}: value ${seed % 100000} status ${seed % 7 == 0 ? "error" : "ok"} ${"x".repeat(seed % 9)}\n`)
  }
  const making = `${path}-${Date.now()}`
  writeFileSync(making, parts.join(""))
  renameSync(making, path)
}

let rest = ""
const lines = new Transform({
  readableObjectMode: true,
  decodeStrings: false,
  transform(chunk: string, encoding, callback) {
    const parts = (rest + chunk).split("\n")
    rest = parts.pop() ?? ""
    for (const line of parts) this.push(line)
    callback()
  },
  flush(callback) {
    if (rest != "") this.push(rest)
    callback()
  },
})
const sevens = new Transform({
  objectMode: true,
  transform(line: string, encoding, callback) {
    if (line.includes("7")) callback(undefined, line.toUpperCase())
    else callback()
  },
})
let count = 0
let total = 0
const counter = new Writable({
  objectMode: true,
  write(line: string, encoding, callback) {
    count++
    total += line.length
    callback()
  },
})
await pipeline(createReadStream(path, "utf8"), lines, sevens, counter)
console.log(count, total)
