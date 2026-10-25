// same-as-node
// node:fs/promises: the functions of node:fs that wait on the thread pool
// and give a Promise, in a directory of the test's own under the temporary
// one, which it removes at the end.
import { readFile, writeFile, appendFile, mkdtemp, mkdir, readdir, stat, lstat, rm, rmdir,
         rename, copyFile, unlink, access, chmod, truncate, realpath, constants } from "node:fs/promises"
import { promises } from "node:fs"
import { join, basename, relative } from "node:path"
import { tmpdir } from "node:os"

const dir = await mkdtemp(join(tmpdir(), "tz-fsp-"))
console.log(basename(dir).length, (await stat(dir)).isDirectory())

// Text in and out, with the encoding and the flags.
const notes = join(dir, "notes.txt")
await writeFile(notes, "first\n")
await appendFile(notes, "second\n")
await writeFile(notes, "third\n", { flag: "a" })
console.log(JSON.stringify(await readFile(notes, "utf8")), (await readFile(notes, { encoding: "utf-8" })).length)
await writeFile(notes, "заменён", "utf8")
console.log(await readFile(notes, { encoding: "utf8", flag: "r" }))
await truncate(notes, 4)
console.log(JSON.stringify(await promises.readFile(notes, "utf8")))

// Directories, and what is in them: recursive readdir goes as Node's async
// one does, deeper directories after the entries of their parent.
console.log(await mkdir(join(dir, "a")), basename((await mkdir(join(dir, "a", "b", "c"), { recursive: true })) ?? "none"))
console.log(await mkdir(join(dir, "a"), { recursive: true }))
await writeFile(join(dir, "a", "f1.txt"), "1")
await writeFile(join(dir, "a", "b", "f2.txt"), "22")
await mkdir(join(dir, "x"))
await writeFile(join(dir, "x", "f3.txt"), "333")
console.log(await readdir(dir))
console.log(await readdir(dir, { recursive: true }))
for (const e of await readdir(dir, { withFileTypes: true, recursive: true }))
  console.log(e.name, relative(dir, e.parentPath), e.isFile(), e.isDirectory())

// What stat tells.
const st = await stat(join(dir, "x", "f3.txt"))
console.log(st.isFile(), st.size, st.mtimeMs > 0, (await lstat(dir)).isDirectory())

// Moving, copying, permissions and deleting.
await rename(join(dir, "x", "f3.txt"), join(dir, "moved.txt"))
await copyFile(join(dir, "moved.txt"), join(dir, "copy.txt"))
await copyFile(join(dir, "moved.txt"), join(dir, "copy.txt"), constants.COPYFILE_FICLONE)
await access(join(dir, "copy.txt"))
console.log(await readFile(join(dir, "copy.txt"), "utf8"))
await chmod(join(dir, "copy.txt"), 0o444)
await chmod(join(dir, "copy.txt"), "644")
await unlink(join(dir, "copy.txt"))
await rmdir(join(dir, "x"))
console.log(await readdir(dir))
console.log((await realpath(dir)).toLowerCase() == dir.toLowerCase())

// Many at once: each waits on the pool, and Promise.all keeps their order.
const names: string[] = []
for (let i = 0; i < 20; i++) names.push(join(dir, `n${i}.txt`))
await Promise.all(names.map((name, i) => writeFile(name, "x".repeat(i))))
const sizes = await Promise.all(names.map(async (name) => (await readFile(name, "utf8")).length))
console.log(sizes.join(","))

await rm(dir, { recursive: true })
await rm(dir, { recursive: true, force: true })
console.log("removed")
