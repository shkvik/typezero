// same-as-node
// Hashes and ciphers named by values known only when the program runs, which the full layer
// looks up; the ciphers only it has (CCM, Camellia, 3DES...); the lists of node.
import {
  createHash, createHmac, createCipheriv, createDecipheriv, getCiphers, getCurves, pbkdf2Sync,
} from "node:crypto"

const names = ["sha256", "sha3-512", "md5", "sm3"]
for (const name of names)
  console.log(name, createHash(name).update("abc").digest("hex"), createHmac(name, "k").update("abc").digest("base64"))
for (const name of ["aes-256-gcm", "aes-128-cbc", "chacha20-poly1305", "camellia-256-cbc", "aes-192-ofb", "des-ede3-cbc"]) {
  const keyLength = name == "des-ede3-cbc" ? 24 : name.startsWith("aes-128") ? 16 : name.startsWith("aes-192") ? 24 : 32
  const ivLength = name.includes("gcm") || name.includes("poly") ? 12 : name.startsWith("des") ? 8 : 16
  const c = createCipheriv(name, Buffer.alloc(keyLength, 5), Buffer.alloc(ivLength, 6))
  const sealed = c.update("hello, world", "utf8", "hex") + c.final("hex")
  const d = createDecipheriv(name, Buffer.alloc(keyLength, 5), Buffer.alloc(ivLength, 6))
  if (name.includes("gcm") || name.includes("poly"))
    d.setAuthTag(c.getAuthTag())
  console.log(name, sealed, d.update(sealed, "hex", "utf8") + d.final("utf8"))
}
const ccm = createCipheriv("aes-128-ccm", Buffer.alloc(16, 1), Buffer.alloc(12, 2), { authTagLength: 16 })
ccm.setAAD(Buffer.from("aad"), { plaintextLength: 5 })
const ccmOut = ccm.update("hello", "utf8", "hex") + ccm.final("hex")
console.log(ccmOut, ccm.getAuthTag().toString("hex"))
const unknown: string = ["foo"][0] ?? ""
for (const f of [
  () => {
    createHash(unknown)
  },
  () => {
    createHmac(unknown, "k")
  },
  () => {
    createCipheriv(unknown, Buffer.alloc(16), Buffer.alloc(16))
  },
  () => {
    pbkdf2Sync("pw", "salt", 1, 16, unknown)
  },
]) {
  try {
    f()
  } catch (e) {
    console.log(e.name, e.message)
  }
}
console.log(getCiphers().length, getCiphers().includes("aes-256-gcm"), getCurves().includes("prime256v1"))
console.log(getCurves().join(","))
