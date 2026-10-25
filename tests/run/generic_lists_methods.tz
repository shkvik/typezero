// A wrapper of a method (docs/generics.md, section 9): the method taken as
// a value is bound to its object, so the wrapper calls it on that object.
// (Not compared with node, where `c.inc` loses its `this`.)
class Counter {
  n = 0
  inc(by: int) {
    this.n += by
    return this.n
  }
}
function logged<A extends unknown[], R>(name: string, fn: (...args: A) => R) {
  return (...args: A): R => {
    console.log("->", name, ...args)
    return fn(...args)
  }
}
const c = new Counter()
const inc = logged("inc", c.inc)
inc(2)
console.log(inc(3), c.n)
// A parameter left out is there, undefined, in the list of the copy.
function greet(name: string, greeting?: string) { return `${greeting ?? "hi"}, ${name}` }
console.log(logged("greet", greet)("ann"))
