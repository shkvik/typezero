// same-as-node
// node:fs: files and directories, in a directory of the test's own under
// the temporary one, which it removes at the end.
import { readFileSync, writeFileSync, appendFileSync, existsSync, mkdirSync, mkdtempSync,
         readdirSync, statSync, lstatSync, rmSync, renameSync, copyFileSync, unlinkSync,
         rmdirSync, realpathSync, accessSync, chmodSync, truncateSync, constants, Stats,
         Dirent } from "node:fs"
import { join, basename, relative } from "node:path"
import { tmpdir } from "node:os"

const dir = mkdtempSync(join(tmpdir(), "tz-files-"))
console.log(basename(dir).length, existsSync(dir), statSync(dir).isDirectory())

// Text in and out, with the encoding and the flags.
const notes = join(dir, "notes.txt")
writeFileSync(notes, "first\n")
appendFileSync(notes, "second\n")
writeFileSync(notes, "third\n", { flag: "a" })
console.log(JSON.stringify(readFileSync(notes, "utf8")), readFileSync(notes, { encoding: "utf-8" }).length)
writeFileSync(notes, "replaced", "utf8")
writeFileSync(notes, "", { flag: "a", encoding: "utf8" })
console.log(readFileSync(notes, { encoding: "utf8", flag: "r" }), existsSync(join(dir, "nope")))

// Directories, and what is in them.
console.log(mkdirSync(join(dir, "a")), mkdirSync(join(dir, "a"), { recursive: true }))
const first = mkdirSync(join(dir, "b", "c", "d"), { recursive: true })
console.log(basename(first ?? "none"))
writeFileSync(join(dir, "b", "c", "x.txt"), "x")
writeFileSync(join(dir, "b", "y.txt"), "yy")
console.log(readdirSync(dir))
console.log(readdirSync(dir, { recursive: true }))
console.log(readdirSync(join(dir, "b"), "utf8"), readdirSync(join(dir, "b", "c", "d")))
for (const entry of readdirSync(dir, { withFileTypes: true, recursive: true }))
  console.log(entry.name, relative(dir, entry.parentPath), entry.isFile(), entry.isDirectory(),
              entry.isSymbolicLink())
const entries: Dirent[] = readdirSync(join(dir, "b"), { withFileTypes: true })
for (const entry of entries.slice(0, 1))
  console.log(Object.keys(entry))

// What statSync tells.
const stats: Stats = statSync(join(dir, "b", "y.txt"))
console.log(stats.isFile(), stats.isDirectory(), stats.isSymbolicLink(), stats.isFIFO(), stats.size,
            stats.mtimeMs > 0, stats.mtimeMs <= Date.now() + 1000, stats.nlink)
console.log(Object.keys(stats))
console.log(lstatSync(dir).isDirectory(), statSync(join(dir, "nope"), { throwIfNoEntry: false }))
console.log((statSync(dir).mode & 0o170000) == 0o040000, (stats.mode & 0o170000) == 0o100000)

// Moving, copying and deleting.
renameSync(join(dir, "b", "y.txt"), join(dir, "z.txt"))
copyFileSync(join(dir, "z.txt"), join(dir, "copy.txt"))
copyFileSync(join(dir, "notes.txt"), join(dir, "copy.txt"))
console.log(readFileSync(join(dir, "copy.txt"), "utf8"), existsSync(join(dir, "b", "y.txt")))
unlinkSync(join(dir, "copy.txt"))
truncateSync(join(dir, "z.txt"), 1)
console.log(JSON.stringify(readFileSync(join(dir, "z.txt"), "utf8")))
truncateSync(join(dir, "z.txt"))
console.log(statSync(join(dir, "z.txt")).size)
accessSync(join(dir, "z.txt"))
accessSync(join(dir, "z.txt"), constants.R_OK | constants.W_OK)
chmodSync(join(dir, "z.txt"), 0o444)
console.log((statSync(join(dir, "z.txt")).mode & 0o200) == 0)
chmodSync(join(dir, "z.txt"), "644")
console.log((statSync(join(dir, "z.txt")).mode & 0o200) != 0)
rmdirSync(join(dir, "a"))
console.log(realpathSync(dir) == dir, realpathSync(join(dir, "b", "..", "z.txt")) == join(dir, "z.txt"))
rmSync(join(dir, "z.txt"))
rmSync(join(dir, "missing"), { force: true })
rmSync(join(dir, "missing"), { recursive: true, force: true })
console.log(readdirSync(dir))
rmSync(dir, { recursive: true })
console.log(existsSync(dir))
