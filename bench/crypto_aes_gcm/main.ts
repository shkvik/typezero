// 2 GiB encrypted with AES-256-GCM: 131072 messages of 16 KiB (a record of TLS), each with a
// nonce of its own; the SHA-256 of their tags is printed.
import { createCipheriv, createHash } from "node:crypto"

const key = Buffer.alloc(32, 7)
const piece = Buffer.alloc(1 << 14)
for (let i = 0; i < piece.length; i++) piece[i] = i & 0xff
const nonce = Buffer.alloc(12)
const tags = createHash("sha256")
for (let i = 0; i < 131072; i++) {
  nonce.writeUInt32BE(i, 0)
  const c = createCipheriv("aes-256-gcm", key, nonce)
  c.update(piece)
  c.final()
  tags.update(c.getAuthTag())
}
console.log(tags.digest("hex"))
