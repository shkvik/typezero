// same-as-node
// A PassThrough where a Writable is expected (a Duplex is one), compose()
// of two Transforms, finished() of node:stream/promises and with a callback,
// addAbortSignal() and _construct(), which the writes wait for.
import { PassThrough, Readable, Transform, Writable, compose, finished as finishedCb, addAbortSignal } from "node:stream"
import { finished } from "node:stream/promises"
const log = (s: string) => console.log(s)
function sinkInto(w: Writable, text: string) {
  w.write(text)
  w.end()
}
const pt = new PassThrough()
pt.on("data", (c) => log(`pt ${c.toString()}`))
sinkInto(pt, "into a duplex")
await finished(pt)
log(`pt finished ${pt.writableFinished} ${pt.readableEnded}`)

const upper = new Transform({
  transform(chunk, enc, cb) {
    cb(undefined, chunk.toString().toUpperCase())
  },
})
const exclaim = new Transform({
  transform(chunk, enc, cb) {
    cb(undefined, `${chunk.toString()}!`)
  },
})
const both = compose(upper, exclaim)
both.on("data", (c) => log(`composed ${c.toString()}`))
both.write("hey")
both.end()
await finished(both)
log("composed done")

const ac = new AbortController()
const slow = new Readable({ read() {} })
addAbortSignal(ac.signal, slow)
slow.on("error", (e) => log(`slow error ${e.name}`))
slow.on("close", () => log("slow close"))
ac.abort()

let constructed = false
const c = new Writable({
  construct(cb) {
    log("construct")
    setTimeout(() => {
      constructed = true
      cb()
    }, 1)
  },
  write(chunk, enc, cb) {
    log(`write after construct ${constructed} ${chunk.toString()}`)
    cb()
  },
})
c.write("x")
c.end()
finishedCb(c, (err) => log(`c finished ${err == undefined}`))
