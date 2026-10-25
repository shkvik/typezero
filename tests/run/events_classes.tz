// same-as-node --experimental-transform-types
// Emitters of generic classes, super.emit() in a class of its own, a field
// whose type gives the map of its emitter, the names of events in constants
// and string enums, and listeners that take `this`, which is the emitter
// (docs/events.md).
import { EventEmitter } from "node:events"

class Store<T> extends EventEmitter<{ change: [value: T, old: T] }> {
  value: T
  constructor(value: T) {
    super()
    this.value = value
  }
  set(value: T) {
    const old = this.value
    this.value = value
    this.emit("change", value, old)
  }
}
const s = new Store(1)
s.on("change", (v, old) => console.log("change", old, "->", v))
s.set(2)
const t = new Store("a")
t.on("change", (v) => console.log("text", v.toUpperCase()))
t.set("b")

class Logged extends EventEmitter<{ data: [n: int] }> {
  log: string[] = []
  send(n: int) {
    this.log.push(`send ${n}`)
    return super.emit("data", n)
  }
}
const l = new Logged()
l.on("data", (n) => l.log.push(`got ${n}`))
console.log(l.send(5), l.log)

type AppEvents = { start: [at: double] }
class App {
  bus: EventEmitter<AppEvents> = new EventEmitter()
}
const app = new App()
app.bus.on("start", (at) => console.log("start", at))
app.bus.emit("start", 1.5)

const DATA = "data"
enum Ev {
  Ping = "ping",
}
const e = new EventEmitter<{ data: [n: int]; ping: [] }>()
e.on(DATA, (n) => console.log("data", n))
e.on(Ev.Ping, () => console.log("ping"))
e.emit(DATA, 1)
e.emit(Ev.Ping)
const o = new EventEmitter<{ constructor: [n: int]; toString: [] }>()
o.on("constructor", (n) => console.log("ctor", n))
o.emit("constructor", 3)
console.log(o.emit("toString"))

class Server extends EventEmitter<{ close: [code: int] }> {
  name = "srv"
}
const server = new Server()
server.on("close", function (this: Server, code) {
  console.log(this.name, code, this == server)
})
function onClose(this: Server, code: int) {
  console.log("named", this.name, code)
}
server.once("close", onClose)
console.log(server.listenerCount("close", onClose))
server.emit("close", 1)
server.emit("close", 2)
server.on("close", onClose)
server.off("close", onClose)
console.log(server.listenerCount("close"), server)
