// Files of bytes: written from typed arrays, Buffers and DataViews, read
// back as a Buffer or as text in the encodings of Buffer.
import { appendFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"

export class Store {
  dir: string
  // What each file held when it was last written, shared with the caller.
  written = new Map<string, Uint8Array>()
  constructor() {
    this.dir = mkdtempSync(join(tmpdir(), "tz-bytes-mix-"))
  }
  path(name: string): string {
    return join(this.dir, name)
  }
  save(name: string, bytes: Uint8Array) {
    writeFileSync(this.path(name), bytes)
    this.written.set(name, bytes)
  }
  saveText(name: string, text: string, encoding: "latin1" | "utf8") {
    if (encoding == "latin1")
      writeFileSync(this.path(name), text, "latin1")
    else
      writeFileSync(this.path(name), text)
  }
  append(name: string, more: Buffer | DataView) {
    appendFileSync(this.path(name), more)
  }
  load(name: string): Buffer {
    return readFileSync(this.path(name))
  }
  hex(name: string): string {
    return readFileSync(this.path(name), "hex")
  }
  text(name: string): string {
    return readFileSync(this.path(name), { encoding: "latin1" })
  }
  async loadLater(name: string): Promise<Buffer> {
    await new Promise<void>(resolve => setTimeout(resolve, 1))
    return this.load(name)
  }
  close() {
    rmSync(this.dir, { recursive: true })
  }
}
