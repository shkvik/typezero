// same-as-node
// Web Crypto: AES-GCM, AES-CBC and AES-CTR with keys and IVs fixed, so that the bytes are node's;
// the counter of AES-CTR wraps in its last bits; AES-KW and the keys wrapKey and unwrapKey move.
const subtle = crypto.subtle
const enc = new TextEncoder()
const dec = new TextDecoder()
const hex = (b: ArrayBuffer) => Buffer.from(b).toString("hex")
const bytes = (n: int, start: int) => {
  const out = new Uint8Array(n)
  for (let i = 0; i < n; i++) out[i] = (start + i) & 0xff
  return out
}
const key = await subtle.importKey("raw", bytes(32, 0), "AES-GCM", true, ["encrypt", "decrypt"])
console.log(key.algorithm)
const iv = bytes(12, 100)
const sealed = await subtle.encrypt({ name: "AES-GCM", iv, additionalData: enc.encode("aad") }, key, enc.encode("hello world"))
console.log("gcm", hex(sealed))
const opened = await subtle.decrypt({ name: "AES-GCM", iv, additionalData: enc.encode("aad") }, key, sealed)
console.log(dec.decode(opened))
const short = await subtle.encrypt({ name: "AES-GCM", iv, tagLength: 96 }, key, enc.encode("hi"))
console.log("gcm96", hex(short))
try {
  await subtle.decrypt({ name: "AES-GCM", iv }, key, sealed)
} catch (e) {
  console.log(e.name, e.message)
}
const cbcKey = await subtle.importKey("raw", bytes(16, 7), { name: "AES-CBC" }, false, ["encrypt", "decrypt"])
const cbc = await subtle.encrypt({ name: "AES-CBC", iv: bytes(16, 1) }, cbcKey, enc.encode("sixteen bytes!!!"))
console.log("cbc", hex(cbc))
console.log(dec.decode(await subtle.decrypt({ name: "AES-CBC", iv: bytes(16, 1) }, cbcKey, cbc)))
try {
  await subtle.decrypt({ name: "AES-CBC", iv: bytes(16, 2) }, cbcKey, cbc.slice(0, 20))
} catch (e) {
  console.log(e.name, e.message)
}
const ctrKey = await subtle.importKey("raw", bytes(24, 3), "AES-CTR", false, ["encrypt", "decrypt"])
const counter = new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0xfe])
const ctr = await subtle.encrypt({ name: "AES-CTR", counter, length: 4 }, ctrKey, bytes(50, 9))
console.log("ctr", hex(ctr))
console.log("ctr back", hex(await subtle.decrypt({ name: "AES-CTR", counter, length: 4 }, ctrKey, ctr)))
const ctr128 = await subtle.encrypt({ name: "AES-CTR", counter, length: 128 }, ctrKey, bytes(40, 9))
console.log("ctr128", hex(ctr128))

// AES-KW, and wrapKey and unwrapKey.
const kek = await subtle.importKey("raw", Buffer.from("000102030405060708090A0B0C0D0E0F", "hex"), "AES-KW", false, ["wrapKey", "unwrapKey"])
const inner = await subtle.importKey("raw", Buffer.from("00112233445566778899AABBCCDDEEFF", "hex"), "AES-GCM", true, ["encrypt", "decrypt"])
const wrapped = await subtle.wrapKey("raw", inner, kek, "AES-KW")
console.log("kw", hex(wrapped))
const back = await subtle.unwrapKey("raw", wrapped, kek, "AES-KW", "AES-GCM", true, ["decrypt"])
console.log(back, hex(await subtle.exportKey("raw", back)))
const wrappedJwk = await subtle.wrapKey("jwk", inner, kek, { name: "AES-KW" })
console.log("kw jwk", wrappedJwk.byteLength)
const backJwk = await subtle.unwrapKey("jwk", wrappedJwk, kek, "AES-KW", { name: "AES-GCM" }, false, ["encrypt"])
console.log(backJwk.algorithm, backJwk.usages)
try { await subtle.unwrapKey("raw", new Uint8Array(24), kek, "AES-KW", "AES-GCM", true, ["decrypt"]) } catch (e) { console.log(e.name, e.message) }
const gcmKey = await subtle.importKey("raw", new Uint8Array(16), "AES-GCM", false, ["wrapKey", "unwrapKey"])
const viaGcm = await subtle.wrapKey("jwk", inner, gcmKey, { name: "AES-GCM", iv: new Uint8Array(12) })
console.log("gcm wrap", hex(viaGcm))
const fromGcm = await subtle.unwrapKey("jwk", viaGcm, gcmKey, { name: "AES-GCM", iv: new Uint8Array(12) }, "AES-GCM", true, ["encrypt"])
console.log(fromGcm.algorithm)

// A typed array of other elements, a DataView and an ArrayBuffer are bytes too.
const any = await subtle.encrypt({ name: "AES-CBC", iv: new DataView(new ArrayBuffer(16)) }, cbcKey, new Uint32Array([1, 2, 3]))
console.log("cbc of u32", hex(any), hex(await subtle.decrypt({ name: "AES-CBC", iv: new ArrayBuffer(16) }, cbcKey, any)))
const g = await subtle.generateKey({ name: "AES-CTR", length: 128 }, true, ["encrypt", "decrypt"])
console.log(g, (await subtle.exportKey("raw", g)).byteLength, await subtle.exportKey("jwk", inner))
