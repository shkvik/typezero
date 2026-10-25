// 50 000 X25519 key exchanges between the same keys; the secret is printed.
import { createPrivateKey, createPublicKey, diffieHellman } from "node:crypto"

const privatePem =
  "-----BEGIN PRIVATE KEY-----\n" +
  "MC4CAQAwBQYDK2VuBCIEILDx6HXrOrgm2PqDbEj9yCzSL0TRCBWhBzmCkJXoAcFU\n" +
  "-----END PRIVATE KEY-----\n"
const publicPem =
  "-----BEGIN PUBLIC KEY-----\n" +
  "MCowBQYDK2VuAyEAUK8OHWoiL3Lxgz3pczoxppdpSzndnKQX85k+QdmW2hk=\n" +
  "-----END PUBLIC KEY-----\n"

const privateKey = createPrivateKey(privatePem)
const publicKey = createPublicKey(publicPem)
let secret = Buffer.alloc(0)
for (let i = 0; i < 50000; i++) secret = diffieHellman({ privateKey, publicKey })
console.log(secret.toString("hex"))
