// same-as-node --experimental-transform-types
// Interfaces: classes that implement them, interfaces that extend others,
// methods in interfaces, and literals of them. An object is of an interface
// type only if its type says so (`implements`, `extends`); code that knows it
// by the interface reads its fields and calls its methods whatever its class.
interface Named {
  name: string
}
interface Greeter extends Named {
  greet(other: string): string
}
interface Aged {
  age: int
  readonly born: int
}

class Person implements Greeter, Aged {
  born: int
  constructor(public name: string, public age: int) {
    this.born = 2026 - age
  }
  greet(other: string) {
    return `${this.name} greets ${other}`
  }
}

class Robot implements Greeter {
  serial = 7
  constructor(public name: string) {}
  greet(other: string) {
    return `${this.name}#${this.serial} beeps at ${other}`
  }
}

// A class that extends one with interfaces has them too; its own fields
// lie after those up the chain, so Aged reads its fields through a table.
class Student extends Robot implements Aged {
  born = 2010
  constructor(name: string, public age: int) {
    super(name)
  }
}

function hello(n: Named) {
  return `hello ${n.name}`
}
function older(a: Aged) {
  return `${a.age + 1} (born ${a.born})`
}

const ann = new Person("Ann", 30)
const r2 = new Robot("R2")
const sam = new Student("Sam", 16)
// A literal of an interface with methods: a function in a field, or a
// method of the literal.
const lit: Greeter = { name: "Lit", greet: (o: string) => `lit greets ${o}` }
const polite: Greeter = {
  name: "Polite",
  greet(other) {
    return `${this.name} bows to ${other}`
  },
}
const greeters: Greeter[] = [ann, r2, sam, lit, polite]
for (const g of greeters) console.log(hello(g), "|", g.greet("you"))
const aged: Aged[] = [ann, sam, { age: 70, born: 1956 }]
for (const a of aged) console.log(older(a))
const names: Named[] = [ann, r2, { name: "plain" }]
console.log(names.map(n => n.name).join(", "))
console.log(greeters)

// instanceof finds the class of an object known by an interface.
for (const g of greeters)
  console.log(g.name, g instanceof Person, g instanceof Robot, g instanceof Student)

// Literals with methods of one interface go in one array.
interface Counter {
  n: int
  inc(): void
  get(): int
}
let step = 2
const counters: Counter[] = [
  { n: 0, inc() { this.n += step }, get() { return this.n } },
  { n: 10, inc() { this.n-- }, get() { return this.n * 100 } },
]
class Real implements Counter {
  n = 5
  inc() {
    this.n *= 2
  }
  get() {
    return -this.n
  }
}
counters.push(new Real())
for (const c of counters) {
  c.inc()
  c.inc()
  console.log(c.get(), c.n)
}
console.log(counters)
