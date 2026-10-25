// ECDSA P-256 signatures with SHA-256, 50000 times;
// they are random, so the last is verified.
import { createPrivateKey, sign, verify } from "node:crypto"

const pem =
  "-----BEGIN PRIVATE KEY-----\n" +
  "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgGw0mYuW3oxSvt0EY\n" +
  "nJvKXzdThD45siaRIzWojBykQX+hRANCAARnoIUMMjzuG6gDJ4WTCCf2YVcEmfeY\n" +
  "Ksmom19AoDLv4h4DMQ7+K0TawHM1PV+PDPnLHU+LD5M3fXSFdGZMKru3\n" +
  "-----END PRIVATE KEY-----\n"

const key = createPrivateKey(pem)
const data = Buffer.from("a message of some length to sign, as a token or a handshake would")
let signature = Buffer.alloc(0)
for (let i = 0; i < 50000; i++) signature = sign("sha256", data, key)
console.log(verify("sha256", data, key, signature))
