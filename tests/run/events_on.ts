// same-as-node
// on() of node:events in 'for await': the events that come before the loop waits
// wait in a queue; an event named in 'close', an 'error' event and an abort
// end it, and 'break' too; each removes the listeners. once() with a timeout.
import { EventEmitter, on, once } from "node:events"
const log = (s: string) => console.log(s)
const bus = new EventEmitter<{ data: [n: int, label: string]; end: []; error: [err: Error] }>()
const it = on(bus, "data", { close: ["end"] })
bus.emit("data", 1, "one")
bus.emit("data", 2, "two")
setTimeout(() => {
  bus.emit("data", 3, "three")
  bus.emit("end")
}, 5)
for await (const [n, label] of it)
  log(`${n} ${label}`)
log(`listeners after: ${bus.listenerCount("data")} ${bus.listenerCount("end")}`)

// an error ends it, after the events before it
const errs = on(bus, "data")
setTimeout(() => {
  bus.emit("data", 4, "four")
  bus.emit("error", new Error("boom"))
}, 5)
try {
  for await (const [n] of errs) log(`got ${n}`)
} catch (e) {
  log(`caught ${e instanceof Error ? e.message : "?"}`)
}
log(`listeners after error: ${bus.listenerCount("data")} ${bus.listenerCount("error")}`)

// break removes the listeners
setTimeout(() => bus.emit("data", 5, "five"), 5)
for await (const [n] of on(bus, "data")) {
  log(`first ${n}`)
  break
}
log(`listeners after break: ${bus.listenerCount("data")}`)

// a signal ends it with an AbortError
const ac = new AbortController()
setTimeout(() => ac.abort(), 5)
try {
  for await (const [n] of on(bus, "data", { signal: ac.signal })) log(`never ${n}`)
} catch (e) {
  log(`aborted: ${e instanceof Error ? e.name : "?"}`)
}
// once with a timeout signal
const alive = setTimeout(() => log("kept alive"), 50)
try {
  await once(bus, "end", { signal: AbortSignal.timeout(5) })
} catch (e) {
  log(`once: ${e instanceof Error ? e.name : "?"} ${e instanceof Error && e.cause instanceof Error ? e.cause.name : "-"}`)
}
clearTimeout(alive)
