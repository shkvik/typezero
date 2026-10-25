// same-as-node
// Transform and PassThrough of node:stream (docs/streams.md): transform()
// and flush(), a Transform of strings to words in object mode, and the
// backpressure of a PassThrough.
import { PassThrough, Transform } from "node:stream"
const log = (s: string) => console.log(s)
const upper = new Transform({
  transform(chunk, encoding, callback) {
    log(`_transform ${chunk.toString()} ${encoding}`)
    callback(undefined, chunk.toString().toUpperCase())
  },
  flush(callback) {
    log("_flush")
    this.push("!")
    callback()
  },
})
upper.on("data", (chunk) => log(`data ${chunk.toString()}`))
upper.on("end", () => log("end"))
upper.on("finish", () => log("finish"))
upper.on("close", () => {
  log("close")
  upper.removeAllListeners()
})
upper.write("hello")
upper.write(" world")
upper.end()

const split = new Transform<string, string>({
  decodeStrings: false,
  readableObjectMode: true,
  transform(chunk, encoding, callback) {
    for (const word of chunk.split(" ")) this.push(word)
    callback()
  },
})
split.on("data", (word) => log(`word ${word} ${typeof word}`))
split.end("a bb ccc")

const pt = new PassThrough({ highWaterMark: 2 })
log(`pt write ${pt.write("abc")}`)
pt.on("drain", () => log("pt drain"))
pt.on("data", (c) => log(`pt data ${c.toString()}`))
pt.end()
pt.on("end", () => {
  log("pt end")
  pt.removeAllListeners()
})
