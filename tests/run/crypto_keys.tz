// same-as-node
// KeyObjects, signatures and key exchange: the full layer, with keys fixed so that what is
// deterministic (Ed25519, RSA with PKCS#1 v1.5, X25519, ECDH) gives the same bytes as node.
import {
  createPrivateKey, createPublicKey, sign, verify, createSign, createVerify, generateKeyPairSync,
  createECDH, diffieHellman, constants, KeyObject,
} from "node:crypto"

const rsaPem = "-----BEGIN PRIVATE KEY-----\nMIICdwIBADANBgkqhkiG9w0BAQEFAASCAmEwggJdAgEAAoGBAK+8SB2syspYQFCc\n11nnTjqdq41RNaYu2EmTxYlse1/+wXLLBcW2FMpHDa89Yxe89YRg0FHai/vg2Y92\nnLxcmi5Fket+BC0kFEvPIFe1URszhVt8SI+7CYu8x7yGuc0dcx1oKZOn6HKwG1vG\nHojPtM8OqvSpjkNdgHfueZQGhvjlAgMBAAECgYEAiEviLsnOOmN59ku+pmbEgKe6\n3w7GKk6Yx3AJvAzcH1bJmIunvXD77OpwzaIx0b5nOTWRPHrhKQh6FE+tPlYf6Uxb\nl9asTqAfhTpl9mAG68IRZyvFZYv1hd7diptUkQC+8JJV9JHgK39j7Yi2EOUqJMJN\nSPRVgMhZFLI9qHFF6l0CQQDV/P5y4A5tDkPXmhfdsDquuEMQMhTRX485OTIkCHEG\nySMzoUlaY+GX204YlKjR7QA981NqP8w1CPWOXhlwEJ3bAkEA0jy18/eql2IxMjBE\nedZbCV2tRmMEpNwvnLok5+Rh/V7454bZrCo69np2SFzJA4BzgaYuojnbBHTikiNa\nPRpgPwJAEZiNH8sWxJbwszDjl4IfQTC7qwBDP/g4ob73RYlAKguuDlfL3VfrO/53\njIPk4eY4J9/zUwBN/WpwLrXEJ7fmEwJBAKFxCXBdIVy6hHi74/mDh8j7bJH+e5R1\nmQJLVwfTswO0KILyZLj2fOzFQYtz5NCRlSFcEbd1oiAb+diq2SLO3FUCQECQreSt\n8/hu0I/1tQk7YkIwLe78V3dQ6/W0LFIw8PFBZcxgeMRWMjPYVD4Q63bovQNkXq0J\nnv+47nddOqQehLI=\n-----END PRIVATE KEY-----\n"
const ecPem = "-----BEGIN PRIVATE KEY-----\nMIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgL5PvfD1BhLinAX7K\nBcGEuQivbZ1ktBRNiJjLYjNUvUmhRANCAAR085NCeBSaDiZrTlVPW0JcvzC2l/VS\ne2lMbPaK7ESPM37MW3yi8igU0oP/ibxpq3mfWRIcuDynYqUwr+FYuwgz\n-----END PRIVATE KEY-----\n"
const edPem = "-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIK5iQ7aYXCPh1AxMVDZeLx2UXgdfnFXNRKLzXlnO/QtF\n-----END PRIVATE KEY-----\n"
const xPem = "-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VuBCIEIIgYOiTyhiTTK0Ev2g3Pn360BhbIyWTj5S2iJjUP+Q9a\n-----END PRIVATE KEY-----\n"
const x2Pem = "-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VuBCIEIAD8IftKz5rFI+J5KcYZ4IwQvEt/gwPl3bcDMIWQWCJ/\n-----END PRIVATE KEY-----\n"
// Keys from PEM, and what they are.
const rsa = createPrivateKey(rsaPem)
const ec = createPrivateKey({ key: ecPem, format: "pem" })
const ed = createPrivateKey(edPem)
const x = createPrivateKey(xPem)
for (const k of [rsa, ec, ed, x])
  console.log(k.type, k.asymmetricKeyType, k.symmetricKeySize)
const rsaPub = createPublicKey(rsa)
const edPub = createPublicKey(edPem)
console.log(rsaPub.type, edPub.asymmetricKeyType, edPub.equals(createPublicKey(ed)), edPub.equals(rsa))

// Back to PEM, DER and JWK, and in again.
console.log(rsa.export({ format: "pem", type: "pkcs8" }) == rsaPem)
console.log(rsa.export({ format: "pem", type: "pkcs1" }))
console.log(rsaPub.export({ format: "pem", type: "spki" }))
console.log(rsaPub.export({ format: "pem", type: "pkcs1" }))
console.log(ec.export({ format: "pem", type: "sec1" }))
console.log(edPub.export({ format: "der", type: "spki" }).toString("hex"))
console.log(JSON.stringify(rsa.export({ format: "jwk" })))
console.log(JSON.stringify(ec.export({ format: "jwk" })))
console.log(JSON.stringify(createPublicKey(ec).export({ format: "jwk" })))
console.log(JSON.stringify(ed.export({ format: "jwk" })))
console.log(JSON.stringify(x.export({ format: "jwk" })))
const fromJwk = createPrivateKey({ key: ec.export({ format: "jwk" }), format: "jwk" })
console.log(fromJwk.equals(ec), createPublicKey({ key: edPub.export({ format: "jwk" }), format: "jwk" }).equals(edPub))
const der = rsa.export({ format: "der", type: "pkcs8" })
console.log(createPrivateKey({ key: der, format: "der", type: "pkcs8" }).equals(rsa))
const locked = rsa.export({ format: "pem", type: "pkcs8", cipher: "aes-256-cbc", passphrase: "secret" })
console.log(locked.split("\n")[0], createPrivateKey({ key: locked, passphrase: "secret" }).equals(rsa))
try {
  createPrivateKey({ key: locked, passphrase: "wrong" })
} catch (e) {
  console.log(e.message)
}

// Signatures: Ed25519 and RSA with PKCS#1 v1.5 give the same bytes each time.
const data = Buffer.from("message to sign")
console.log(sign(undefined, data, ed).toString("hex"))
console.log(sign("sha256", data, rsa).toString("base64"))
console.log(sign("sha256", "message to sign", rsaPem).toString("base64"))
console.log(verify(undefined, data, edPub, sign(undefined, data, ed)))
console.log(verify("sha256", data, rsaPub, sign("sha256", data, rsa)), verify("sha256", data, rsaPub, Buffer.alloc(128)))
const ecSig = sign("sha384", data, ec)
console.log(verify("sha384", data, createPublicKey(ec), ecSig), verify("sha256", data, createPublicKey(ec), ecSig))
const p1363 = sign("sha256", data, { key: ec, dsaEncoding: "ieee-p1363" })
console.log(p1363.length, verify("sha256", data, { key: createPublicKey(ec), dsaEncoding: "ieee-p1363" }, p1363))
const pss = sign("sha256", data, { key: rsa, padding: constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 })
console.log(pss.length, verify("sha256", data, { key: rsaPub, padding: constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 }, pss))
const signer = createSign("SHA256")
signer.update("message ").update("to sign")
const signature = signer.sign(rsa, "hex")
console.log(signature == sign("sha256", data, rsa).toString("hex"))
const verifier = createVerify("SHA256")
verifier.update(data)
console.log(verifier.verify(rsaPub, signature, "hex"))
try {
  sign("sha256", data, rsaPub)
} catch (e) {
  console.log(e.message)
}

// Generated keys: their kinds and encodings.
const pair = generateKeyPairSync("ed25519")
console.log(pair.publicKey.type, pair.privateKey.asymmetricKeyType)
console.log(verify(undefined, data, pair.publicKey, sign(undefined, data, pair.privateKey)))
const encoded = generateKeyPairSync("ec", {
  namedCurve: "P-384",
  publicKeyEncoding: { type: "spki", format: "pem" },
  privateKeyEncoding: { type: "pkcs8", format: "der" },
})
console.log(encoded.publicKey.split("\n")[0], encoded.privateKey.length > 100)
const rsaPair = generateKeyPairSync("rsa", { modulusLength: 1024, publicKeyEncoding: { type: "pkcs1", format: "pem" } })
console.log(rsaPair.publicKey.split("\n")[0], rsaPair.privateKey.type)
const jwkPair = generateKeyPairSync("x25519", { publicKeyEncoding: { format: "jwk" }, privateKeyEncoding: { format: "jwk" } })
console.log(jwkPair.publicKey.kty, jwkPair.privateKey.crv)

// Key exchange: X25519 and ECDH with keys fixed.
const other = createPrivateKey(x2Pem)
console.log(diffieHellman({ privateKey: x, publicKey: createPublicKey(other) }).toString("hex"))
const alice = createECDH("prime256v1"), bob = createECDH("prime256v1")
alice.setPrivateKey(Buffer.alloc(32, 1))
bob.setPrivateKey(Buffer.alloc(32, 2))
console.log(alice.getPublicKey("hex"))
console.log(alice.getPublicKey("base64", "compressed"), alice.getPrivateKey("hex"))
console.log(alice.computeSecret(bob.getPublicKey()).toString("hex"))
console.log(bob.computeSecret(alice.getPublicKey("hex"), "hex", "hex"))
const fresh = createECDH("secp256k1")
console.log(fresh.generateKeys().length, fresh.getPublicKey(undefined, "compressed").length)
try {
  alice.computeSecret(Buffer.from("0400", "hex"))
} catch (e) {
  console.log(e.message)
}
try {
  createECDH("nonsense")
} catch (e) {
  console.log(e.message)
}

// KeyObject as a type.
const k: KeyObject = rsaPub
console.log(k.type)

// Last: node leaves an error of OpenSSL behind here for the next call to find.
console.log(edPub.equals(rsaPub))
