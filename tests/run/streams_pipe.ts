// pipe() with backpressure (write() false, 'drain', pause and resume of the
// source), pipeline() of streams with its callback and an error that
// destroys them all, and finished(), in node's order of events. undefined
// ends a stream, as null does in node: the output is node's with push(null).
import { PassThrough, Readable, Writable, pipeline, finished } from "node:stream"
const log = (s: string) => console.log(s)
let i = 0
const src = new Readable({
  highWaterMark: 4,
  read() {
    i++
    if (i <= 5) this.push(`part${i};`)
    else this.push(undefined)
  },
})
const got: string[] = []
const dst = new Writable({
  highWaterMark: 6,
  write(chunk, enc, cb) {
    got.push(chunk.toString())
    log(`write ${chunk.toString()}`)
    setImmediate(() => cb())
  },
})
src.on("pause", () => log("src pause"))
src.on("resume", () => log("src resume"))
dst.on("pipe", () => log("dst pipe"))
dst.on("unpipe", () => log("dst unpipe"))
dst.on("drain", () => log("dst drain"))
src.on("end", () => log("src end"))
dst.on("finish", () => {
  log(`dst finish ${got.join("")}`)
})
src.pipe(dst)

const a = new PassThrough()
const b = new PassThrough()
const c = new Writable({
  write(chunk, enc, cb) {
    log(`c got ${chunk.toString()}`)
    cb()
  },
})
const last = pipeline(a, b, c, (err) => log(`pipeline done ${err == undefined}`))
log(`last is c ${last == c}`)
finished(c, (err) => log(`finished c ${err?.message ?? "ok"}`))
a.write("x")
a.end("y")

const e1 = new PassThrough()
const e2 = new PassThrough()
pipeline(e1, e2, (err) => log(`pipeline error ${err?.message} e1=${e1.destroyed} e2=${e2.destroyed}`))
e1.destroy(new Error("bad"))
