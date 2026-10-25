// More listeners of one event than the limit: a warning in stderr, once
// for each array of listeners, with node's text, on the next tick
// (docs/events.md, section 4). node runs its tests with --no-warnings, so
// this one is tz's alone.
// expect-stderr: MaxListenersExceededWarning: Possible EventEmitter memory leak detected. 11 data listeners added to [EventEmitter]. MaxListeners is 10. Use emitter.setMaxListeners() to increase limit
// expect-stderr: MaxListenersExceededWarning: Possible EventEmitter memory leak detected. 2 a listeners added to [Server]. MaxListeners is 1. Use emitter.setMaxListeners() to increase limit
import { EventEmitter } from "node:events"

const bus = new EventEmitter<{ data: [n: int] }>()
for (let i = 0; i < 11; i++) bus.on("data", () => {})
bus.on("data", () => {})
console.log("added", bus.listenerCount("data"))
class Server extends EventEmitter<{ a: [] }> {}
const s = new Server()
s.setMaxListeners(1)
s.on("a", () => {})
s.on("a", () => {})
// A limit of 0 is none.
s.setMaxListeners(0)
for (let i = 0; i < 20; i++) s.on("a", () => {})
console.log("done", s.listenerCount("a"))
