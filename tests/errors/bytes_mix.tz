// Binary data with node:fs, unions and node:buffer: what a check cannot
// tell, and what node:fs cannot take.
import { readFileSync, writeFileSync, readdirSync } from "node:fs"
// expect-error: 'Blob' of 'node:buffer' is not supported yet; tz has Buffer
import { Buffer, Blob } from "node:buffer"

const file = "data.bin"
const bytes = new Uint8Array([1, 2, 3])
// expect-error: an encoding is for text; 'writeFileSync' writes the bytes as they are: drop the encoding
writeFileSync(file, bytes, "hex")
// expect-error: 'writeFileSync' writes a string or bytes (a Uint8Array or a Buffer); found 'ArrayBuffer'; write a view of it, e.g. 'new Uint8Array(buffer)'
writeFileSync(file, new ArrayBuffer(2))
// expect-error: 'readdirSync' gives names in utf8 only; drop the encoding 'latin1'
readdirSync(".", "latin1")
// expect-error: unknown encoding 'utf9' (node throws a TypeError)
readFileSync(file, "utf9")

const buffer = Buffer.from("x")
// expect-error: the value is a 'Buffer', so 'instanceof Uint8Array' is always true
console.log(buffer instanceof Uint8Array)
// expect-error: a 'Uint8Array' is never a 'Float64Array', so 'instanceof Float64Array' is always false
console.log(bytes instanceof Float64Array)
const either: string | Date = process.argv.length > 9 ? "s" : new Date(0)
// expect-error: a 'string | Date' is never a 'Uint8Array': none of its members is one, so this is always false
console.log(either instanceof Uint8Array)
// `Uint8Array | Buffer` is a Uint8Array.
const all: Uint8Array | Buffer = process.argv.length > 9 ? bytes : buffer
// expect-error: the value is a 'Uint8Array', so 'instanceof Uint8Array' is always true
console.log(all instanceof Uint8Array)
const maybe: Uint8Array | undefined = process.argv.length > 9 ? undefined : bytes
// expect-error: the value is a 'Uint8Array' or undefined; test it with '!= undefined'
console.log(maybe instanceof Uint8Array)
const mixed: Uint8Array | string = process.argv.length > 9 ? "s" : bytes
if (mixed instanceof Uint8Array)
  console.log(mixed.length)
else
  console.log(mixed.toUpperCase())
// A Uint8Array may be a Buffer, which only the check tells.
if (mixed instanceof Buffer)
  console.log(mixed.toString("hex"))
// expect-error: property 'toString' does not exist on 'Uint8Array | string'
console.log(mixed.toString("hex"))
