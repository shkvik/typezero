// Calls on emitters are checked against their event maps (docs/events.md).
import { EventEmitter, on, errorMonitor } from "node:events"

interface User {
  name: string
}
const typed = new EventEmitter<{ data: [chunk: string]; end: []; user: [user: User] }>()
// expect-error: the emitter has no event 'dat': its event map has 'data', 'end' and 'user'; did you mean 'data'?
typed.on("dat", () => {})
// expect-error: type 'int' is not assignable to type 'string'
typed.emit("data", 5)
// expect-error: the listeners of 'end' get '[]': 0 arguments, but got 1
typed.emit("end", 1)
// expect-error: parameter 'chunk' is declared as 'int', but the function gets 'string' here
typed.on("data", (chunk: int) => {})
function twice(n: int) {
  return n * 2
}
// expect-error: the listeners of 'user' get '[User]', but this function is '(int) => int'
typed.on("user", twice)
// expect-error: a listener is a function, but this is 'int'
typed.on("end", 5)
// expect-error: the function is declared to return 'void', but returns 'int'
typed.on("end", async () => 1)
const name = String(Math.random())
// expect-error: the name of the event must be known when compiling, as a string literal
typed.emit(name)
// expect-error: 'on' of an emitter can only be called
const f = typed.on
// expect-error: 'emit' takes no type arguments
typed.emit<int>("end")
// expect-error: event 'data' has 'string', which is not a tuple
const bad = new EventEmitter<{ data: string }>()
// expect-error: event 'end' is optional, but an event is in the map or not
const optional = new EventEmitter<{ end?: [] }>()
// expect-error: every emitter has the event 'newListener', whose listeners get the name of the event and the listener
const service = new EventEmitter<{ newListener: [n: int] }>()
// expect-error: 'EventEmitter' needs its event map here, e.g. 'EventEmitter<{ data: [chunk: string]; end: [] }>', or the type of an emitter, e.g. 'typeof bus'
let plain: EventEmitter
// expect-error: an emitter that captures rejections emits 'error' with the Error of a rejected async listener, but its event map gives 'error' '[int]'
const capture = new EventEmitter<{ error: [code: int] }>({ captureRejections: true })
// expect-error: 'on' takes an emitter of node:events, but this is 'int'
on(5, "data")
// expect-error: 'on' of node:events takes an emitter of node:events; the events of an EventTarget are not supported yet
on(new EventTarget(), "data")
// expect-error: 'Object.values' of an emitter is not supported
Object.values(typed)
// expect-error: 'once' takes an emitter of node:events or an EventTarget, but this is 'int'
void EventEmitter.once(5, "data")
// expect-error: cannot find name '__tz_pid'
__tz_pid()

const target = new EventTarget()
// expect-error: a listener of an EventTarget takes an Event, or a kind of it such as 'CustomEvent<int>', but this one takes 'int'
target.addEventListener("x", (n: int) => {})
async function count(n: int) {
  return n
}
// expect-error: an async listener returns 'Promise<void>', but this one returns 'Promise<int>'
typed.on("user", count)
class Other {
  n = 1
}
// expect-error: the listener takes 'this' of type 'Other', but it gets the emitter, of type 'EventEmitter<{ data: [string]; end: []; user: [User] }>'
typed.on("end", function (this: Other) {})
// expect-error: an async listener cannot take 'this'; use the emitter by name
typed.on("end", async function (this: unknown) {})
