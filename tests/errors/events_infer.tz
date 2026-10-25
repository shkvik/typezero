// An emitter without an event map has the events its emit() calls give it
// (docs/events.md, section 2): a listener of an event that no code emits
// is a typo, an event emitted with two types is an error that names both
// places, and a name known only when the program runs needs a map.
import { EventEmitter } from "node:events"

const bus = new EventEmitter()
// expect-error: no code of the program emits 'usr' on this emitter; it emits 'user' and 'count'; did you mean 'user'?
bus.on("usr", () => {})
bus.emit("user", "ann")
bus.emit("count", 1)
// expect-error: 'count' is emitted with [int] at events_infer.ts:11:10, but with 'string' here; an event has one type of arguments
bus.emit("count", "two")
// expect-error: 'count' is emitted with [int] at events_infer.ts:11:10, but with 0 arguments here
bus.emit("count")
const topic = String(Math.random())
// expect-error: the name of the event must be known when compiling, as a string literal
bus.emit(topic, 1)

class Quiet extends EventEmitter {}
const quiet = new Quiet()
// expect-error: no code of the program emits 'ping' on this emitter, nor any other event: an emitter without an event map has the events that emit() calls give it
quiet.on("ping", () => {})
