// The chunk types of node:stream (docs/streams.md, section 2): setEncoding()
// on a stream that went elsewhere before, or where its type would depend on
// a branch; chunks of another type in push(), write() and pipe(); an
// object-mode stream whose type nothing tells.
import { PassThrough, Readable, Writable, compose, pipeline } from "node:stream"
import { createReadStream } from "node:fs"
class User {
  constructor(public name: string) {}
}
function use(r: Readable) {
  r.on("end", () => {})
}
const a = createReadStream("a.txt")
a.on("data", (b) => console.log(b.length))
// expect-error: setEncoding() on a stream that is used elsewhere already, which sees its chunks as Buffers, but strings would come: it got listeners (on()) before this
a.setEncoding("utf8")
const b = createReadStream("b.txt")
use(b)
// expect-error: it went elsewhere (passed, stored or returned) before this
b.setEncoding("utf8")
const c = createReadStream("c.txt")
// expect-error: but this setEncoding() is in a block of its own (an if, a loop, a function)
if (process.argv.length > 2) c.setEncoding("utf8")
let d = createReadStream("d.txt")
// expect-error: but 'd' may hold another stream later; declare it with 'const'
d.setEncoding("utf8")
const e = createReadStream("e.txt")
setTimeout(() => e.resume(), 1)
// expect-error: a function written before uses it before this
e.setEncoding("utf8")
const objects = new Readable<User>({ objectMode: true, read() {} })
// expect-error: setEncoding() decodes the Buffers of a stream of bytes, and this is an object-mode stream of User
objects.setEncoding("utf8")
// expect-error: type 'string' is not assignable to type 'User | undefined'
objects.push("not a user")
const bytes = new Readable({ read() {} })
// expect-error: 'push' of a stream of Buffer takes a string, a Buffer or a Uint8Array, but this is 'int'
bytes.push(42)
const strings = new Writable<string>({ decodeStrings: false, write(c, e, cb) { cb() } })
// expect-error: 'write' of a stream of string takes a string, but this is 'Buffer'
strings.write(Buffer.from("x"))
// expect-error: pipe() writes chunks of User to 'PassThrough<Buffer, Buffer>', which takes Buffer
objects.pipe(new PassThrough())
// expect-error: cannot infer the type of the chunks of this object-mode Readable: no code of the program pushes into it
const nothing = new Readable({ objectMode: true, read() {} })
// expect-error: parameter 's' is declared as 'string', but the function gets 'Buffer' here
bytes.on("data", (s: string) => console.log(s))
// expect-error: pipeline() writes chunks of User to 'PassThrough<Buffer, Buffer>', which takes Buffer
pipeline(objects, new PassThrough(), (err) => {})
// expect-error: compose() takes streams, and this is 'int'
compose(bytes, 5)
// expect-error: 'Readable.from' takes an iterable, an async iterable, a string or a Buffer, but this is 'int'
Readable.from(42)
// expect-error: 'Writable<string>' has no push(): it is not readable
strings.push("x")
// A method the stream calls, with other parameters: another variant of the
// name, which the stream would never call.
class WrongChunks extends Writable {
  // expect-error: '_write' is what node:stream calls, as '_write(chunk: Buffer, encoding: string, callback: (Error?) => void)'
  _write(chunk: string, encoding: string, callback: () => void) {}
}
class WrongSize extends Readable {
  // expect-error: '_read' is what node:stream calls, as '_read(size: int)'
  _read(size: string) {}
}
