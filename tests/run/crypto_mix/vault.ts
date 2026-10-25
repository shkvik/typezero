// What main.ts keeps its secrets with: a generic sealed box over any value
// that JSON writes, AES-256-GCM with a key from scrypt, and a store of the
// sealed bytes in files read and written through node:fs/promises, which
// emits an event for each.
import { EventEmitter } from "node:events"
import { createCipheriv, createDecipheriv, createHash, randomBytes, scryptSync } from "node:crypto"
import { readFile, writeFile } from "node:fs/promises"
import { join } from "node:path"

export function keyFrom(password: string, salt: Uint8Array) {
  return scryptSync(password, salt, 32)
}

// iv (12 bytes), tag (16 bytes), then the ciphertext.
export function seal<T>(key: Buffer, value: T): Buffer {
  const iv = randomBytes(12)
  const cipher = createCipheriv("aes-256-gcm", key, iv)
  const body = Buffer.concat([cipher.update(JSON.stringify(value), "utf8"), cipher.final()])
  return Buffer.concat([iv, cipher.getAuthTag(), body])
}

export function open(key: Buffer, sealed: Uint8Array): Json {
  const bytes = Buffer.from(sealed)
  const decipher = createDecipheriv("aes-256-gcm", key, bytes.subarray(0, 12))
  decipher.setAuthTag(bytes.subarray(12, 28))
  return JSON.parse(decipher.update(bytes.subarray(28), undefined, "utf8") + decipher.final("utf8"))
}

export class Vault extends EventEmitter {
  constructor(readonly dir: string, readonly key: Buffer) {
    super()
  }

  async put<T>(name: string, value: T) {
    const sealed = seal(this.key, value)
    await writeFile(join(this.dir, name), sealed)
    this.emit("stored", name, createHash("sha256").update(sealed).digest("hex").length)
  }

  async get(name: string) {
    return open(this.key, await readFile(join(this.dir, name)))
  }
}
