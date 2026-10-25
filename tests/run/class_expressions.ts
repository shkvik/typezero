// Class expressions where tz differs from JavaScript, or where JavaScript
// has nothing to compare with (docs/decorators.md, section 5): an
// expression is one class, however many times it runs, so its static fields
// get their values the first time; each copy of a generic function, or of
// a generic class, makes its own; the members of such a class take
// decorators.

let made = 0
function makeCounter() {
  return class Counter {
    static instances = 0
    count = 0
    constructor() {
      Counter.instances++
      made++
    }
    later() {
      return () => ++this.count
    }
  }
}
const A = makeCounter()
const B = makeCounter()
const a = new A()
new B()
const tick = a.later()
tick()
console.log(A == B, A.instances, B.instances, made, tick(), a)
A.instances = 10
console.log(B.instances)

// In a generic class, each instance of it makes its own class.
class Base {
  base = true
}
class Box<T> {
  constructor(public value: T) {}
  holder() {
    return class Holder extends Base {
      items: T[] = []
    }
  }
}
const IntHolder = new Box(1).holder()
const StringHolder = new Box("s").holder()
const ints = new IntHolder()
ints.items.push(2)
console.log(ints, new StringHolder(), IntHolder == StringHolder)

// Decorators on the members of a class expression run when it is made.
const seen: string[] = []
function Track(_cls: object, name: string) {
  seen.push(name)
}
function WithStatus<B extends Constructor>(Base: B) {
  return class extends Base {
    @Track status = "ok"
    @Track check() {
      return this.status
    }
  }
}
class Service {}
const Checked = WithStatus(Service)
console.log(new Checked().check(), seen)
