// same-as-node
// `for await` over a Readable, which a break destroys and an error ends,
// and Readable.from() of an array, an async generator and a string.
import { PassThrough, Readable } from "node:stream"
const log = (s: string) => console.log(s)
const pt = new PassThrough()
pt.write("one ")
pt.write("two ")
setTimeout(() => pt.end("three"), 1)
let text = ""
for await (const chunk of pt) {
  log(`chunk ${chunk.toString()}`)
  text += chunk.toString()
}
log(`text ${text} destroyed ${pt.destroyed}`)

const words = Readable.from(["alpha", "beta", "gamma"])
for await (const w of words) {
  log(`word ${w.toUpperCase()}`)
  if (w == "beta") break
}
log(`after break destroyed ${words.destroyed}`)

async function* gen() {
  yield 1
  yield 2
  yield 3
}
const nums = Readable.from(gen())
let sum = 0
for await (const n of nums) sum += n
log(`sum ${sum}`)

const one = Readable.from("whole string")
one.on("data", (s) => log(`one ${s} ${typeof s}`))
const bad = new PassThrough()
setTimeout(() => bad.destroy(new Error("broken")), 1)
try {
  for await (const c of bad) log(`never ${c.length}`)
} catch (e) {
  log(`caught ${e instanceof Error ? e.message : "?"}`)
}
