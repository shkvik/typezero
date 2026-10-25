// same-as-node
// Web Crypto as jose and hono write it: algorithms from functions that name their types as
// TypeScript's lib.dom does (Algorithm, KeyAlgorithm, RsaPssParams...: one type in tz), a hash
// in a template, a generateKey of an algorithm known only when the program runs.
function subtleAlgorithm(alg: string): Algorithm {
  const hash = `SHA-${alg.slice(-3)}`
  switch (alg) {
    case "HS256":
    case "HS384":
    case "HS512":
      return { hash, name: "HMAC" }
    case "PS256":
    case "PS384":
    case "PS512":
      return { hash, name: "RSA-PSS", saltLength: parseInt(alg.slice(-3), 10) >> 3 }
    case "ES256":
      return { hash, name: "ECDSA", namedCurve: "P-256" }
    case "EdDSA":
      return { name: "Ed25519" }
  }
  throw new Error(`alg ${alg} is not supported`)
}
// hono's way: a union of the TS names, which are all one type here.
function getKeyAlgorithm(name: string): KeyAlgorithm | RsaHashedImportParams | RsaPssParams | EcKeyImportParams {
  if (name == "HS256") return { name: "HMAC", hash: { name: "SHA-256" } }
  return { name: "ECDSA", namedCurve: "P-256", hash: { name: "SHA-256" } }
}
const enc = new TextEncoder()
async function signJws(alg: string, secret: string, input: string) {
  const usage: KeyUsage = "sign"
  const key = await crypto.subtle.importKey("raw", enc.encode(secret), { hash: `SHA-${alg.slice(-3)}`, name: "HMAC" }, false, [usage])
  return new Uint8Array(await crypto.subtle.sign(subtleAlgorithm(alg), key, enc.encode(input)))
}
const sig = await signJws("HS256", "secret", "header.payload")
console.log(Buffer.from(sig).toString("base64url"))
const k = await crypto.subtle.importKey("raw", enc.encode("secret"), getKeyAlgorithm("HS256"), false, ["sign"])
console.log(Buffer.from(await crypto.subtle.sign(getKeyAlgorithm("HS256"), k, enc.encode("header.payload"))).toString("base64url"))
const pair = await crypto.subtle.generateKey(subtleAlgorithm("ES256"), false, ["sign", "verify"])
if (!(pair instanceof CryptoKey)) console.log(pair.privateKey.type, pair.publicKey.algorithm)
