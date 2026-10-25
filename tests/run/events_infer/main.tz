// same-as-node
// Event maps found from the emit() calls of the whole program
// (docs/events.md, section 2): of `new EventEmitter()`, of a class that
// extends EventEmitter, through fields, parameters and other files. The
// listeners come first here; the emits that give them their types follow,
// in functions, methods and another file, and an event may have the types
// a listener of another one passes.
import { EventEmitter, once } from "node:events"
import { Shop, report } from "./shop.ts"

class User {
  name: string
  constructor(name: string) {
    this.name = name
  }
}

const bus = new EventEmitter()
bus.on("user", (user) => console.log("user", user.name))
bus.on("order", (order) => console.log("order", order.id, order.items.length, order.note ?? "-"))
bus.on("total", (sum, currency) => console.log("total", sum.toFixed(2), currency))
bus.once("count", (n, label) => console.log("count", n * 2, label))
bus.on("chain", (n) => bus.emit("chained", `${n}!`))
bus.on("chained", (text) => console.log("chained", text))

function checkout(items: string[]) {
  bus.emit("order", { id: 7, items, note: items.length > 1 ? "many" : undefined })
  bus.emit("total", items.length * 2.5, "EUR")
}
// The emitter as a parameter: the type of the variable.
function welcome(target: typeof bus, name: string) {
  target.emit("user", new User(name))
}

bus.emit("user", new User("ann"))
welcome(bus, "bob")
checkout(["tea", "cake"])
// An int and a double: the event passes doubles.
bus.emit("count", 21, "x")
bus.emit("count", 2.5, "y")
bus.emit("chain", 3)
console.log(bus.eventNames())

class App {
  events = new EventEmitter()
  start() {
    this.events.emit("ready", { at: 1 })
  }
}
const app = new App()
app.events.on("ready", (info) => console.log("ready at", info.at))
app.start()

const shop = new Shop()
shop.on("sold", (what, count) => console.log("sold", what, count))
shop.sell("apple", 3)
report(shop)
setTimeout(() => shop.emit("closed"), 1)
await once(shop, "closed")
console.log("closed", shop.eventNames())
