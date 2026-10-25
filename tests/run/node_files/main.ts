// same-as-node
// Each file imports what it uses of the built-in modules, under its own
// names.
import { existsSync as exists, mkdtempSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import { saveNote, readNote } from "./notes.ts"

const dir = mkdtempSync(join(tmpdir(), "tz-notes-"))
const file = saveNote(dir, "today", "купить хлеб")
console.log(exists(file), readNote(file))
rmSync(dir, { recursive: true })
console.log(exists(dir))
