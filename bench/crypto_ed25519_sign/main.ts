// Ed25519 signatures, 50000 times;
// the SHA-256 of the last signature is printed.
import { createPrivateKey, sign, createHash } from "node:crypto"

const pem =
  "-----BEGIN PRIVATE KEY-----\n" +
  "MC4CAQAwBQYDK2VwBCIEILv4fsSzEiPk7kwDzHM70NuGeJRFZkyyEOjHEB0ttb//\n" +
  "-----END PRIVATE KEY-----\n"

const key = createPrivateKey(pem)
const data = Buffer.from("a message of some length to sign, as a token or a handshake would")
let signature = Buffer.alloc(0)
for (let i = 0; i < 50000; i++) signature = sign(undefined, data, key)
console.log(createHash("sha256").update(signature).digest("hex"))
