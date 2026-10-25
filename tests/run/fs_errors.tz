// same-as-node
// What fails in node:fs throws a SystemError with the code, number, call
// and paths Node gives, and Node's text; the directory of the test shows as
// D. Wrong arguments are TypeErrors.
import { readFileSync, writeFileSync, appendFileSync, mkdirSync, mkdtempSync, readdirSync,
         statSync, lstatSync, rmSync, renameSync, copyFileSync, unlinkSync, rmdirSync,
         realpathSync, accessSync, chmodSync, truncateSync, existsSync, constants } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"

const dir = mkdtempSync(join(tmpdir(), "tz-errors-"))
const file = join(dir, "file.txt")
const sub = join(dir, "sub")
const missing = join(dir, "missing")
writeFileSync(file, "text")
mkdirSync(sub)
writeFileSync(join(sub, "inside.txt"), "inside")

function show(what: string, run: () => void) {
  try {
    run()
    console.log(what, "-> no error")
  } catch (e) {
    if (e instanceof SystemError)
      console.log(what, "->", e.name, e.code, e.errno, e.syscall, (e.path ?? "-").replaceAll(dir, "D"),
                  (e.dest ?? "-").replaceAll(dir, "D"), "|", e.message.replaceAll(dir, "D"))
    else
      console.log(what, "->", e.name, "|", e.message)
  }
}

show("read missing", () => readFileSync(missing, "utf8"))
show("read directory", () => readFileSync(sub, "utf8"))
show("read through file", () => readFileSync(join(file, "x"), "utf8"))
show("read empty path", () => readFileSync("", "utf8"))
show("read zero char", () => readFileSync("a\u0000b", "utf8"))
show("write missing dir", () => writeFileSync(join(missing, "x.txt"), "x"))
show("write exclusive", () => writeFileSync(file, "x", { flag: "wx" }))
show("write bad flag", () => writeFileSync(file, "x", { flag: "q" }))
show("write directory", () => writeFileSync(sub, "x"))
show("append missing dir", () => appendFileSync(join(missing, "x.txt"), "x"))
show("mkdir existing", () => mkdirSync(sub))
show("mkdir existing file", () => mkdirSync(file, { recursive: true }))
show("mkdir missing parent", () => mkdirSync(join(missing, "a")))
show("mkdir under file", () => mkdirSync(join(file, "a", "b"), { recursive: true }))
show("mkdtemp missing", () => mkdtempSync(join(missing, "t-")))
show("rmdir not empty", () => rmdirSync(sub))
show("rmdir file", () => rmdirSync(file))
show("rmdir missing", () => rmdirSync(missing))
show("unlink directory", () => unlinkSync(sub))
show("unlink missing", () => unlinkSync(missing))
show("rm directory", () => rmSync(sub))
show("rm missing", () => rmSync(missing))
show("rm missing recursive", () => rmSync(missing, { recursive: true }))
show("rename missing", () => renameSync(missing, join(dir, "other")))
show("rename onto directory", () => renameSync(file, sub))
show("copy missing", () => copyFileSync(missing, join(dir, "other")))
show("copy exclusive", () => copyFileSync(file, join(sub, "inside.txt"), constants.COPYFILE_EXCL))
show("copy directory", () => copyFileSync(sub, join(dir, "other")))
show("copy clone force", () => copyFileSync(file, join(dir, "other"), constants.COPYFILE_FICLONE_FORCE))
show("readdir file", () => readdirSync(file))
show("readdir missing", () => readdirSync(missing))
show("stat missing", () => statSync(missing))
show("lstat missing", () => lstatSync(missing))
show("stat through file", () => statSync(join(file, "x")))
show("realpath missing", () => realpathSync(join(missing, "deeper")))
show("access missing", () => accessSync(missing))
show("chmod missing", () => chmodSync(missing, 0o644))
show("truncate missing", () => truncateSync(missing))
show("copy onto itself", () => copyFileSync(file, file))
console.log(existsSync("a\u0000b"), existsSync(""))

rmSync(dir, { recursive: true })
console.log(existsSync(dir))
