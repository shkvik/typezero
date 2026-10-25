// same-as-node
// process.stdin as a Readable, with setEncoding(), and process.stdout as a
// Writable: what is piped and written comes in order with console.log.
import { Transform } from "node:stream"
process.stdin.setEncoding("utf8")
let text = ""
process.stdin.on("data", (s) => {
  text += s
})
process.stdin.on("end", () => {
  console.log(`got ${text.length} chars: ${JSON.stringify(text)}`)
  const upper = new Transform({
    transform(chunk, enc, cb) {
      cb(undefined, chunk.toString().toUpperCase())
    },
  })
  upper.pipe(process.stdout)
  upper.write("piped ")
  upper.end("to stdout\n")
  upper.on("end", () => {
    process.stdout.write(Buffer.from("bytes\n"))
    console.log("after bytes")
    process.stderr.write("to stderr\n")
  })
})
