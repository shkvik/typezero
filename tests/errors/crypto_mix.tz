// node:crypto with the rest of the language: its objects in unions, as
// streams, and its callback forms, which tz does not have.
import { Hash, Hmac, createHash, createHmac, pbkdf2Sync } from "node:crypto"

// expect-error: 'Hash' and 'Hmac' cannot be told apart when the program runs: both are objects of node:crypto
let either: Hash | Hmac = createHash("sha1")
const mac = createHmac("sha256", "key")
// expect-error: 'on' of Hmac is not supported yet: in node it is a stream too, and tz has no streams yet (docs/streams.md); use 'update(data)' and 'digest()'
mac.on("data", (chunk: Buffer) => console.log(chunk))
async function derive() {
  // Fine: what the Sync form gives, in async code.
  await undefined
  return pbkdf2Sync("pw", "salt", 1000, 32, "sha256")
}
console.log(either, await derive())
