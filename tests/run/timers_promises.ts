// same-as-node
// node:timers/promises: setTimeout and setImmediate give a Promise of their
// value once the time is up, setInterval an async iterator of it; with a
// signal they take the turns of the queue Node's take, and an abort rejects
// them with an AbortError whose cause is the reason.
import { setTimeout as wait, setImmediate as soon, setInterval as every, scheduler } from "node:timers/promises"

const log = (s: string) => console.log(s)
// A chain of microtasks, to show the turns each takes.
void Promise.resolve().then(() => log("m1")).then(() => log("m2")).then(() => log("m3"))
  .then(() => log("m4")).then(() => log("m5"))
log(`timeout ${await wait(10, "value")}`)
const n: int = await wait(1, 42)
log(`int ${n + 1}`)
await wait()
log(`immediate ${await soon(true)}`)
await scheduler.wait(5)
await scheduler.yield()

// Against the timers of the globals: the same lists, so a promise of 20 ms
// set after a callback of 20 ms comes after it.
setTimeout(() => log("callback 20"), 20)
void wait(20).then(() => log("promise 20"))
setTimeout(() => log("callback 10"), 10)
await wait(40)

// With a signal: the finally() of Node takes its turns too.
const ok = new AbortController()
void wait(5, "with signal", { signal: ok.signal }).then((v) => log(v))
void wait(5, "without").then((v) => log(v))
void Promise.resolve().then(() => log("x1")).then(() => log("x2")).then(() => log("x3"))
await wait(20)

// Aborted before, and while waiting.
const done = new AbortController()
done.abort()
try {
  await wait(10, 1, { signal: done.signal })
} catch (e) {
  log(`${e.name}: ${e.message} (${e.cause?.message})`)
}
const later = new AbortController()
void wait(10).then(() => later.abort(new Error("enough")))
try {
  await soon(undefined, { signal: later.signal }).then(() => wait(1000, 0, { signal: later.signal }))
} catch (e) {
  log(`${e.name}: ${e.message} (${e.cause?.message})`)
}

// An interval as an async iterator: `break` stops it.
let count = 0
for await (const tick of every(5, "tick")) {
  count++
  log(`${tick} ${count}`)
  if (count == 3) break
}
const stop = new AbortController()
void wait(25).then(() => stop.abort())
let ticks = 0
try {
  for await (const _ of every(10, undefined, { signal: stop.signal })) ticks++
} catch (e) {
  log(`${e.name} after ${ticks > 0}`)
}

// An unref()'d timer does not keep the program alive.
void wait(10000, undefined, { ref: false }).then(() => log("never"))
log("end")
