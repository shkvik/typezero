// same-as-node
// The dates of a Stats, as Node's getters make them: from the milliseconds
// next to them, rounded; a new date each time, which console.log and
// JSON.stringify do not show, as they are no fields.
import { mkdtempSync, rmSync, statSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"

const dir = mkdtempSync(join(tmpdir(), "tz-dates-"))
const file = join(dir, "a.txt")
const before = Date.now()
writeFileSync(file, "hello")
const st = statSync(file)
const dates = [st.atime, st.mtime, st.ctime, st.birthtime]
const ms = [st.atimeMs, st.mtimeMs, st.ctimeMs, st.birthtimeMs]
console.log(dates.map((d, i) => d.getTime() == Math.round(ms[i] ?? 0)).join())
console.log(st.mtime.getTime() >= before - 2000, st.mtime.getTime() <= Date.now() + 2000)
console.log(st.mtime.toISOString().length, typeof st.mtime.getFullYear())
const json = JSON.stringify(st)
console.log(json.includes("mtimeMs"), json.includes('"mtime"'), Object.keys(st).includes("mtime"))
const sorted = [st.birthtime, st.mtime].sort((a, b) => a.getTime() - b.getTime())
console.log(sorted.length, sorted[0]!.getTime() <= sorted[1]!.getTime())
rmSync(dir, { recursive: true })
