// same-as-node
// Web Crypto: PBKDF2 and HKDF, deriveBits and deriveKey (with the RFC 5869 vectors), and keys
// of every family generated, whose bytes are random: their algorithms, usages and lengths.
const subtle = crypto.subtle
const enc = new TextEncoder()
const hex = (b: ArrayBuffer) => Buffer.from(b).toString("hex")

const pw = await subtle.importKey("raw", enc.encode("password"), "PBKDF2", false, ["deriveBits", "deriveKey"])
console.log(pw)
console.log("pbkdf2", hex(await subtle.deriveBits({ name: "PBKDF2", salt: enc.encode("salt"), iterations: 1000, hash: "SHA-256" }, pw, 256)))
console.log("pbkdf2 sha1", hex(await subtle.deriveBits({ name: "PBKDF2", salt: enc.encode("salt"), iterations: 2, hash: "SHA-1" }, pw, 160)))
const aes = await subtle.deriveKey({ name: "PBKDF2", salt: enc.encode("salt"), iterations: 1000, hash: "SHA-512" }, pw, { name: "AES-CBC", length: 256 }, true, ["encrypt"])
console.log(aes, hex(await subtle.exportKey("raw", aes)))
const ikm = await subtle.importKey("raw", new Uint8Array(22).fill(0x0b), "HKDF", false, ["deriveBits", "deriveKey"])
const salt = Buffer.from("000102030405060708090a0b0c", "hex")
const info = Buffer.from("f0f1f2f3f4f5f6f7f8f9", "hex")
console.log("hkdf", hex(await subtle.deriveBits({ name: "HKDF", hash: "SHA-256", salt, info }, ikm, 336)))
const mac = await subtle.deriveKey({ name: "HKDF", hash: "SHA-256", salt, info }, ikm, { name: "HMAC", hash: "SHA-256" }, true, ["sign"])
console.log(mac, hex(await subtle.exportKey("raw", mac)))
const mac2 = await subtle.deriveKey({ name: "HKDF", hash: "SHA-384", salt, info }, ikm, { name: "HMAC", hash: "SHA-1", length: 128 }, false, ["verify"])
console.log(mac2.algorithm)
try { await subtle.deriveBits({ name: "HKDF", hash: "SHA-256", salt, info }, ikm) } catch (e) { console.log(e.name, e.message) }
try { await subtle.deriveBits({ name: "PBKDF2", salt, iterations: 1, hash: "SHA-256" }, pw, 12) } catch (e) { console.log(e.name, e.message) }
try { await subtle.deriveBits({ name: "HKDF", hash: "SHA-1", salt, info }, ikm, 255 * 160 + 8) } catch (e) { console.log(e.name, e.message) }

// Generated keys.
const g1 = await subtle.generateKey({ name: "HMAC", hash: "SHA-384" }, true, ["sign", "verify", "sign"])
console.log(g1, (await subtle.exportKey("raw", g1)).byteLength)
const g2 = await subtle.generateKey({ name: "AES-GCM", length: 192 }, false, ["decrypt", "encrypt"])
console.log(g2)
const g3 = await subtle.generateKey({ name: "ECDSA", namedCurve: "P-384" }, true, ["sign", "verify"])
console.log(g3.publicKey, g3.privateKey.usages, (await subtle.exportKey("raw", g3.publicKey)).byteLength)
const s3 = await subtle.sign({ name: "ECDSA", hash: "SHA-512" }, g3.privateKey, enc.encode("x"))
console.log(s3.byteLength, await subtle.verify({ name: "ECDSA", hash: "SHA-512" }, g3.publicKey, s3, enc.encode("x")))
const g4 = await subtle.generateKey({ name: "RSA-OAEP", modulusLength: 1024, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-1" }, true, ["encrypt", "decrypt"])
console.log(g4.publicKey.algorithm, g4.privateKey.usages)
const c4 = await subtle.encrypt("RSA-OAEP", g4.publicKey, enc.encode("secret"))
console.log(new TextDecoder().decode(await subtle.decrypt("RSA-OAEP", g4.privateKey, c4)))
const g5 = await subtle.generateKey("Ed25519", false, ["sign", "verify"])
console.log(g5.privateKey.extractable, g5.publicKey.extractable, g5.publicKey.usages)
const g6 = await subtle.generateKey({ name: "X25519" }, true, ["deriveKey"])
console.log(g6.privateKey.usages, g6.publicKey.usages)
const g7 = await subtle.generateKey({ name: "ECDH", namedCurve: "P-521" }, true, ["deriveBits"])
console.log((await subtle.deriveBits({ name: "ECDH", public: g7.publicKey }, g7.privateKey, 528)).byteLength)
