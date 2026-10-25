// Methods in object literals: each literal with methods is an instance of a
// class of its own.
// expect-error: parameter 'x' needs a type annotation
const untyped = { f(x) { return x } }
// expect-error: the object literal does not fit '{ f: () => int }': 'f' is a field there that code may replace, which a method cannot be; write it as a field: 'f: (...) => ...'
const typed: { f: () => int } = { f() { return 1 } }
// expect-error: the object literal does not fit '{ g: () => int }': it has no field 'f'; an object must have exactly the fields of its type
const other: { g: () => int } = { f() { return 1 } }
const a = { n: 1, get() { return this.n } }
const b = { n: 2, get() { return this.n } }
const c = { n: 3, get() { return `${this.n}` } }
const both = [a, c]
// expect-error: the members of '{ n: int; get() } | { n: int; get() }' have methods 'get' of different types, '() => int' and '() => string'; narrow it first
both[0].get()
// expect-error: an object with methods cannot be spread into an object literal yet; copy the fields by name
const copy = { ...a }
// expect-error: 'Object.values' of an object with methods is not supported; read the fields by name
const values = Object.values(a)
// expect-error: an object literal with methods cannot take the fields of another object with '...'; write them out
const spread = { ...{ n: 1 }, get() { return 1 } }
// expect-error: 'get' is a method of '{ n: int; get() }' and cannot be replaced
a.get = () => 5
const o = {
  n: 0,
  inc() {
    function inner() {
      // expect-error: a 'function' has no 'this' of its own; use an arrow function, which sees the 'this' of the method around it
      return this.n
    }
    return inner()
  },
}
