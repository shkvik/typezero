// same-as-node
// Web Crypto: what node rejects when the program runs, as node does: DOMExceptions of its names,
// legacy codes and messages, and the TypeErrors of Web IDL. The algorithms come from variables:
// written in the call, these would be errors of the compiler (tests/errors/webcrypto.ts).
const subtle = crypto.subtle
const enc = new TextEncoder()
async function attempt(what: string, run: () => Promise<void>) {
  try {
    await run()
    console.log(what, "ok")
  } catch (e) {
    if (e instanceof DOMException) console.log(what, e.name, e.code, e.message)
    else console.log(what, e.name, e.message)
  }
}
const names = ["HMAC", "AES-XYZ", "aes-gcm"]
const hmac = await subtle.importKey("raw", enc.encode("k"), { name: "HMAC", hash: "SHA-256" }, false, ["verify"])
const aes = await subtle.importKey("raw", new Uint8Array(16), "AES-GCM", true, ["encrypt"])
const cbc = await subtle.importKey("raw", new Uint8Array(16), "AES-CBC", false, ["encrypt", "decrypt"])
const pb = await subtle.importKey("raw", enc.encode("pw"), "PBKDF2", false, ["deriveKey"])
await attempt("unknown name", async () => { await subtle.digest(names[1] ?? "", new Uint8Array(1)) })
await attempt("name case", async () => { await subtle.importKey("raw", new Uint8Array(16), names[2] ?? "", false, ["encrypt"]) })
await attempt("no usage", async () => { await subtle.sign("HMAC", hmac, new Uint8Array(1)) })
await attempt("wrong key", async () => { await subtle.encrypt({ name: "AES-CBC", iv: new Uint8Array(16) }, aes, new Uint8Array(1)) })
await attempt("cbc iv", async () => { await subtle.encrypt({ name: "AES-CBC", iv: new Uint8Array(8) }, cbc, new Uint8Array(1)) })
await attempt("gcm short", async () => { await subtle.decrypt({ name: "AES-GCM", iv: new Uint8Array(12) }, cbc, new Uint8Array(4)) })
await attempt("not extractable", async () => { await subtle.exportKey("raw", hmac) })
await attempt("bad der", async () => { await subtle.importKey("spki", new Uint8Array(10), { name: "ECDSA", namedCurve: "P-256" }, true, ["verify"]) })
await attempt("aes length", async () => { await subtle.importKey("raw", new Uint8Array(10), "AES-GCM", true, ["encrypt"]) })
await attempt("jwk alg", async () => { await subtle.importKey("jwk", { kty: "oct", k: "AAAAAAAAAAAAAAAAAAAAAA", alg: "A256GCM" }, "AES-GCM", true, ["encrypt"]) })
await attempt("jwk ext", async () => { await subtle.importKey("jwk", { kty: "oct", k: "AAAAAAAAAAAAAAAAAAAAAA", ext: false }, "AES-GCM", true, ["encrypt"]) })
await attempt("jwk ops", async () => { await subtle.importKey("jwk", { kty: "oct", k: "AAAAAAAAAAAAAAAAAAAAAA", key_ops: ["decrypt"] }, "AES-GCM", false, ["encrypt"]) })
await attempt("jwk kty", async () => { await subtle.importKey("jwk", { kty: "EC", k: "AAAAAAAAAAAAAAAAAAAAAA" }, "AES-GCM", false, ["encrypt"]) })
await attempt("jwk use", async () => { await subtle.importKey("jwk", { kty: "oct", k: "AAAAAAAAAAAAAAAAAAAAAA", use: "sig" }, "AES-GCM", false, ["encrypt"]) })
let alg = { name: "HMAC" }
await attempt("missing hash", async () => { await subtle.importKey("raw", new Uint8Array(4), alg, false, ["sign"]) })
let big = { name: "AES-GCM", length: 1.0 / 0 }
await attempt("infinite", async () => { await subtle.generateKey(big, false, ["encrypt"]) })
await attempt("derive usage", async () => { await subtle.deriveBits({ name: "PBKDF2", salt: new Uint8Array(1), iterations: 1, hash: "SHA-1" }, pb, 8) })
await attempt("iterations", async () => { await subtle.deriveKey({ name: "PBKDF2", salt: new Uint8Array(1), iterations: 0, hash: "SHA-1" }, pb, { name: "AES-GCM", length: 128 }, false, ["encrypt"]) })
const pair = await subtle.generateKey({ name: "ECDH", namedCurve: "P-256" }, false, ["deriveBits"])
await attempt("public of ecdh", async () => { await subtle.deriveBits({ name: "ECDH", public: pair.privateKey }, pair.privateKey, 8) })
await attempt("export pbkdf2", async () => { await subtle.exportKey("raw", pb) })
await attempt("spki of secret", async () => { await subtle.exportKey("spki", aes) })
await attempt("ecdh too long", async () => { await subtle.deriveBits({ name: "ECDH", public: pair.publicKey }, pair.privateKey, 1024) })
const ed = await subtle.generateKey("Ed25519", true, ["sign", "verify"])
await attempt("raw private", async () => { await subtle.exportKey("raw", ed.privateKey) })
await attempt("ed raw len", async () => { await subtle.importKey("raw", new Uint8Array(31), "Ed25519", true, ["verify"]) })
await attempt("ok", async () => { await subtle.importKey("raw", await subtle.exportKey("raw", ed.publicKey), "Ed25519", true, ["verify"]) })
const ctr = await subtle.importKey("raw", new Uint8Array(16), "AES-CTR", false, ["encrypt"])
await attempt("ctr wraps", async () => { await subtle.encrypt({ name: "AES-CTR", counter: new Uint8Array(16), length: 1 }, ctr, new Uint8Array(40)) })
await attempt("ctr fits", async () => { await subtle.encrypt({ name: "AES-CTR", counter: new Uint8Array(16), length: 1 }, ctr, new Uint8Array(32)) })
const usages: KeyUsage[] = []
await attempt("no usages", async () => { await subtle.importKey("raw", new Uint8Array(16), names[2] ?? "", false, usages) })
await attempt("ecdh usage", async () => { await subtle.generateKey({ name: "ECDH", namedCurve: "P-256" }, false, usages) })
let rsa = { name: "RSASSA-PKCS1-v1_5", modulusLength: 1024, publicExponent: new Uint8Array([1, 0, 0, 0, 0, 1]), hash: "SHA-256" }
await attempt("exponent", async () => { await subtle.generateKey(rsa, false, ["sign"]) })
