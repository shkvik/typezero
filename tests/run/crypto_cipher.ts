// same-as-node
// AES-GCM, CBC and CTR and ChaCha20-Poly1305 of the direct layer. crypto_cipher_aesni and
// crypto_cipher_soft run the same with the CPU features OpenSSL may use turned off.
import { createCipheriv, createDecipheriv } from "node:crypto"

const key = Buffer.alloc(32, 7)
const iv = Buffer.alloc(12, 9)
const text = "Сообщение, которое шифруют: " + "abc".repeat(30)

// AES-GCM, each length of key, in pieces and with AAD.
const gcm = createCipheriv("aes-256-gcm", key, iv)
gcm.setAAD(Buffer.from("header"))
const part1 = gcm.update(text.slice(0, 10), "utf8")
const part2 = gcm.update(text.slice(10), "utf8")
const last = gcm.final()
const sealed = Buffer.concat([part1, part2, last])
const tag = gcm.getAuthTag()
console.log(sealed.toString("hex"))
console.log(tag.toString("hex"), part1.length, part2.length, last.length)
const open = createDecipheriv("aes-256-gcm", key, iv)
open.setAAD(Buffer.from("header"))
open.setAuthTag(tag)
console.log(open.update(sealed, undefined, "utf8") + open.final("utf8"))

const g128 = createCipheriv("aes-128-gcm", key.subarray(0, 16), Buffer.alloc(16, 1), { authTagLength: 8 })
console.log(g128.update("short", "utf8", "hex") + g128.final("hex"), g128.getAuthTag().toString("hex"))
const g192 = createCipheriv("aes-192-gcm", key.subarray(0, 24), Buffer.alloc(7, 2))
console.log(g192.update("x".repeat(1000), "utf8", "base64").length + g192.final("base64").length,
            g192.getAuthTag().toString("hex"))

const bad = createDecipheriv("aes-256-gcm", key, iv)
bad.setAuthTag(Buffer.alloc(16))
bad.update(sealed)
try {
  bad.final()
} catch (e) {
  console.log(e.message)
}

// CBC with padding, without it, and the pieces a decipher holds back.
const cbc = createCipheriv("aes-256-cbc", key, Buffer.alloc(16, 3))
const c1 = cbc.update(text, "utf8", "hex") + cbc.final("hex")
console.log(c1)
const dc = createDecipheriv("aes-256-cbc", key, Buffer.alloc(16, 3))
const bytes = Buffer.from(c1, "hex")
const pieces = [dc.update(bytes.subarray(0, 16)), dc.update(bytes.subarray(16, 40)), dc.update(bytes.subarray(40))]
console.log(pieces.map(p => p.length).join(","), dc.final().length)
const raw = createCipheriv("aes-128-cbc", key.subarray(0, 16), Buffer.alloc(16))
raw.setAutoPadding(false)
console.log(raw.update(Buffer.alloc(32, 1)).toString("hex"), raw.final().length)
const raw2 = createCipheriv("aes128", key.subarray(0, 16), Buffer.alloc(16))
raw2.setAutoPadding(false)
raw2.update(Buffer.alloc(5))
try {
  raw2.final()
} catch (e) {
  console.log(e.message)
}
const wrong = createDecipheriv("aes-256-cbc", key, Buffer.alloc(16, 4))
wrong.update(bytes)
try {
  wrong.final()
} catch (e) {
  console.log(e.message)
}

// CTR across pieces of any length.
const ctr = createCipheriv("aes-192-ctr", key.subarray(0, 24), Buffer.alloc(16, 0xff))
const s1 = ctr.update("abc", "utf8", "hex")
const s2 = ctr.update("x".repeat(70), "utf8", "hex")
console.log(s1 + s2 + ctr.final("hex"))
const ctr2 = createDecipheriv("aes-192-ctr", key.subarray(0, 24), Buffer.alloc(16, 0xff))
console.log(ctr2.update(s1 + s2, "hex", "utf8"))

// ChaCha20-Poly1305.
const cc = createCipheriv("chacha20-poly1305", key, iv, { authTagLength: 16 })
cc.setAAD(Buffer.from("aad"))
const ccOut = Buffer.concat([cc.update(text, "utf8"), cc.final()])
console.log(ccOut.toString("base64"), cc.getAuthTag().toString("hex"))
const cd = createDecipheriv("chacha20-poly1305", key, iv, { authTagLength: 16 })
cd.setAAD(Buffer.from("aad"))
cd.setAuthTag(cc.getAuthTag())
console.log(cd.update(ccOut).toString("utf8") == text)
cd.final()

// Errors of the arguments and of the order of calls.
for (const f of [
  () => {
    createCipheriv("aes-256-gcm", Buffer.alloc(31), iv)
  },
  () => {
    createCipheriv("aes-256-cbc", key, iv)
  },
  () => {
    createCipheriv("chacha20-poly1305", key, Buffer.alloc(8))
  },
  () => {
    createCipheriv("aes-256-gcm", key, iv).getAuthTag()
  },
  () => {
    createCipheriv("aes-256-cbc", key, Buffer.alloc(16)).setAAD(Buffer.alloc(1))
  },
  () => {
    createDecipheriv("aes-256-gcm", key, iv).setAuthTag(Buffer.alloc(3))
  },
  () => {
    createDecipheriv("aes-256-gcm", key, iv, { authTagLength: 8 }).setAuthTag(Buffer.alloc(16))
  },
]) {
  try {
    f()
  } catch (e) {
    console.log(e.name, e.message)
  }
}
const done = createCipheriv("aes-256-ctr", key, Buffer.alloc(16))
done.final()
try {
  done.final()
} catch (e) {
  console.log(e.message)
}
try {
  done.update("x")
} catch (e) {
  console.log(e.message)
}

export const ciphered = true
