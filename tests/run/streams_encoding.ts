// setEncoding() makes the chunks strings from that point on (the type of
// the stream changes there, docs/streams.md, section 2), keeping a character
// that a chunk cuts until the next; an encoding at creation gives strings
// too. undefined ends a stream, as null does in node: the output is node's
// with push(null).
import { PassThrough, Readable } from "node:stream"
const log = (s: string) => console.log(s)
const r = new Readable({ read() {} })
r.push(Buffer.from([0xd0, 0xbf, 0xd1]))
r.setEncoding("utf8")
r.push(Buffer.from([0x80, 0xd0, 0xb8]))
r.push(undefined)
r.on("data", (s) => log(`data ${JSON.stringify(s)} ${typeof s} ${s.length}`))
r.on("end", () => log("end"))
const pt = new PassThrough()
pt.setEncoding("hex")
pt.on("data", (s) => log(`hex ${s.toUpperCase()}`))
pt.end("AB")
const t = new PassThrough({ encoding: "utf8" })
t.write(Buffer.from([0xe2, 0x82]))
t.end(Buffer.from("€").subarray(2))
for await (const s of t) log(`euro ${s} ${s.length}`)
