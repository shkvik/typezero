import { writeFileSync, readFileSync } from "fs"
import { join as joinPath, basename } from "path"

export function saveNote(dir: string, name: string, text: string) {
  const file = joinPath(dir, `${name}.txt`)
  writeFileSync(file, text)
  return file
}

export function readNote(file: string) {
  return `${basename(file, ".txt")}: ${readFileSync(file, "utf8")}`
}
