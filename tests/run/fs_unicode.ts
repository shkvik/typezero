// same-as-node
// Names of files in any language, and text in them: the system gets them in
// UTF-16 on Windows, so nothing depends on the code page.
import { readFileSync, writeFileSync, existsSync, mkdirSync, mkdtempSync, readdirSync, statSync,
         renameSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"

const dir = mkdtempSync(join(tmpdir(), "tz-юникод-"))
const folder = join(dir, "папка ✓")
mkdirSync(folder)
const names = ["файл.txt", "😀 smile.txt", "日本語.md", "Ελληνικά", "ß.txt"]
for (const name of names)
  writeFileSync(join(folder, name), `${name}: текст 😀\n`)
console.log(readdirSync(folder).sort())
for (const name of names)
  console.log(existsSync(join(folder, name)), statSync(join(folder, name)).size,
              JSON.stringify(readFileSync(join(folder, name), "utf8")))
renameSync(join(folder, "файл.txt"), join(folder, "переименован.txt"))
console.log(existsSync(join(folder, "файл.txt")), readFileSync(join(folder, "переименован.txt"), "utf8"))
try {
  readFileSync(join(folder, "нет такого.txt"), "utf8")
} catch (e) {
  console.log(e.message.replaceAll(dir, "D"))
}
rmSync(dir, { recursive: true })
console.log(existsSync(dir))
