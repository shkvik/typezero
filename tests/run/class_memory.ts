// Instances are reference counted like any object; a method gets its object
// lent by the caller, who keeps it alive during the call even when the
// method replaces what held it. The test runs with leak checks.
class Item {
  tags: string[] = []
  constructor(public name: string) {}
  rename(name: string) {
    this.name = `${name}!`
    return this
  }
  label() {
    return this.name
  }
}

// The object a method is called on is replaced while the method runs.
class Holder {
  item = new Item("first")
  swap() {
    this.item = new Item("second")
    return "swapped"
  }
  swapInner() {
    return this.item.rename("inner").label()
  }
}
const h = new Holder()
console.log(h.item.label(), h.swap(), h.item.label())
class Owner {
  holder = new Holder()
  replace() {
    this.holder = new Holder()
    return this.holder.swap()
  }
  replaceThroughField() {
    return this.holder.swap()
  }
}
const o = new Owner()
console.log(o.replace(), o.replaceThroughField(), o.holder.item.label())

let current = new Item("global")
class Resetter {
  reset() {
    current = new Item("reset")
    return "done"
  }
}
const resetter = new Resetter()
console.log(current.label(), resetter.reset(), current.label())

// Temporaries, chains, and objects nothing keeps.
console.log(new Item("temp").rename("x").label())
new Item("discarded")
for (let i = 0; i < 100000; i++) new Holder().swap()

// A bound method keeps its object alive.
const items = [new Item("a"), new Item("b")]
const rename = items[0]!.rename
items.length = 0
console.log(rename("kept").label(), items.length)

// Closures that hold `this`, and objects that hold closures.
class Listener {
  heard: string[] = []
  constructor(public name: string) {}
  handler() {
    return (event: string) => {
      this.heard.push(`${this.name}: ${event}`)
    }
  }
}
const handlers: ((event: string) => void)[] = []
for (const name of ["x", "y"]) handlers.push(new Listener(name).handler())
handlers.forEach(f => f("click"))
console.log(handlers.length)

// The variables an object literal's methods capture live in the object.
function counter() {
  let n = 0
  const label = "counter"
  return {
    inc() {
      n++
      return n
    },
    show() {
      return `${label} ${n}`
    },
  }
}
const c = counter()
c.inc()
c.inc()
console.log(c.show())

// A long list of instances: freeing it takes no stack.
class Cell {
  next?: Cell
  constructor(public value: int) {}
}
let head: Cell | undefined = undefined
for (let i = 0; i < 1000000; i++) {
  const cell = new Cell(i)
  cell.next = head
  head = cell
}
let total = 0
for (let cell = head; cell != undefined; cell = cell.next) total += cell.value % 3
console.log(total)
head = undefined

// Static fields hold their objects until the program ends.
class Cache {
  static last = new Item("cached")
  static items: Item[] = []
  static remember(item: Item) {
    Cache.items.push(item)
    Cache.last = item
  }
}
Cache.remember(new Item("one"))
Cache.remember(new Item("two"))
console.log(Cache.last.label(), Cache.items.length)
