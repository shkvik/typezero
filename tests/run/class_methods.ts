// Methods as values, arrows in fields, and methods of object literals. A
// method taken as a value is bound to its object, so it works wherever it
// goes (in JavaScript `const f = o.m; f()` loses `this`).
class Counter {
  count = 0
  step: int
  constructor(step: int) {
    this.step = step
  }
  inc() {
    this.count += this.step
    return this.count
  }
  // An arrow in a field that nothing assigns again is compiled as a method.
  reset = () => {
    this.count = 0
  }
  // A field that holds a function and is replaced below stays a field.
  format: (n: int) => string = n => `#${n}`
  show() {
    return this.format(this.count)
  }
}

const a = new Counter(1)
const b = new Counter(10)
const inc = a.inc
inc()
inc()
console.log(a.count, inc == a.inc, a.inc == a.inc, a.inc == b.inc)

// Bound methods as callbacks.
const steps = [1, 2, 3].map(() => b.inc())
console.log(steps)
const incs = [a.inc, b.inc]
console.log(incs.map(f => f()))
function twice(f: () => int) {
  f()
  return f()
}
console.log(twice(a.inc), twice(b.inc))

// Arrow fields: a method that prints as a field, and a replaced function.
const reset = a.reset
reset()
console.log(a.count, a)
console.log(a.show())
a.format = n => `count ${n}`
console.log(a.show(), Object.keys(a))

// An arrow in a method sees the method's `this`.
class Scaler {
  constructor(public factor: int) {}
  all(values: int[]) {
    return values.map(v => v * this.factor)
  }
  later() {
    return () => this.factor * 100
  }
}
const s = new Scaler(3)
console.log(s.all([1, 2, 3]))
const later = s.later()
s.factor = 4
console.log(later())

// Static methods as values.
class MathUtil {
  static square(x: int) {
    return x * x
  }
  static total = 0
  static add(x: int) {
    MathUtil.total += x
    return MathUtil.total
  }
}
const square = MathUtil.square
console.log([1, 2, 3].map(MathUtil.square), square(9))
console.log([5, 6].map(MathUtil.add), MathUtil.total)

// Methods in object literals: the object is `this`, and the methods may use
// the variables around the literal.
function makeStack(name: string) {
  let pushes = 0
  return {
    items: [0],
    push(n: int) {
      this.items.push(n)
      pushes++
      return this
    },
    top() {
      return this.items[this.items.length - 1] ?? -1
    },
    describe() {
      return `${name}: ${this.items.length} items after ${pushes} pushes`
    },
  }
}
const stack = makeStack("main")
stack.push(1).push(2).push(3)
console.log(stack.top(), stack.describe())
console.log(stack)
const describe = stack.describe
console.log(describe())
const greeter = {
  greeting: "hi",
  greet(who: string) {
    return `${this.greeting}, ${who}`
  },
}
console.log(greeter.greet("Ann"), ["Bob", "Cid"].map(greeter.greet))
greeter.greeting = "hello"
console.log(greeter.greet("Ann"))
