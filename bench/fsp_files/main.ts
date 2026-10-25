// 10 thousand small files read at once with readFile of node:fs/promises
// and Promise.all: each waits on the thread pool. The files are made once,
// in a directory of the temporary one that later runs reuse, so that the
// runs measure the reading.
import { readFile } from "node:fs/promises"
import { existsSync, mkdirSync, writeFileSync, renameSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"

const count = 10000
const dir = join(tmpdir(), "tz-bench-fsp-files")
if (!existsSync(dir)) {
  const making = `${dir}-${process.argv.length}-${Date.now()}`
  mkdirSync(making)
  for (let i = 0; i < count; i++)
    writeFileSync(join(making, `f${i}.txt`), `file ${i}: ${"x".repeat(i % 100)}\n`)
  renameSync(making, dir)
}
const names: string[] = []
for (let i = 0; i < count; i++) names.push(join(dir, `f${i}.txt`))
const texts = await Promise.all(names.map((name) => readFile(name, "utf8")))
let total = 0
for (const text of texts) total += text.length
console.log(texts.length, total)
