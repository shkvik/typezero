// Generic functions: errors in their bodies, checked once with opaque type
// parameters, are reported there; what our rules reject for the types a
// call gives (and TypeScript accepts) is reported at that call.
interface Dog { name: string }
const rex: Dog = { name: "Rex" }

function twice<T>(x: T) {
  // expect-error: operator '*' cannot be applied to types 'T' and 'int'
  return x * 2
}
function sum<T extends number>(xs: T[]) {
  // expect-error: type 'int' is not assignable to type 'T'
  let s: T = 0
  return s
}
function size<T>(x: T) {
  // expect-error: property 'length' does not exist on type 'T'
  return x.length
}
// expect-error: '||' cannot test a value of type 'T | undefined': 'T' may be a number, and 0 would count as false; give a default with '??': 'xs[0] ?? d'
function firstOr<T>(xs: T[], d: T) { return xs[0] || d }
function present<T>(x: T) {
  // expect-error: a value of type 'T' cannot be a condition: 'T' may be a number, and 0 would count as false; check for undefined: 'x != undefined'
  if (x) return 1
  return 0
}

// Our rules for the types of a call.
function show<T>(label: string, x: T) { console.log(label + ": " + x) }
show("n", 5)
// expect-error: cannot call 'show' with T = Dog: an object cannot be converted to a string
show("p", rex)
function less<T>(a: T, b: T) { return a < b }
less("a", "b")
// expect-error: cannot call 'less' with T = Dog: operator '<' cannot be applied to types 'Dog' and 'Dog'
less(rex, rex)
function sorted<T>(xs: T[]) { return xs.sort() }
sorted(["b", "a"])
// expect-error: cannot call 'sorted' with T = int: 'sort()' without a comparison function compares numbers as strings
sorted([3, 1, 2])
function outer<T>(x: T) { show("in outer", x) }
outer(1)
// expect-error: cannot call 'outer' with T = Dog: an object cannot be converted to a string
outer(rex)
// A written constraint moves the error to the signature.
function show2<T extends string | number | boolean>(label: string, x: T) { console.log(label + ": " + x) }
show2("n", 5)
// expect-error: type 'Dog' does not satisfy the constraint 'string | number | boolean' of 'T' in 'show2'
show2("p", rex)

// Type arguments that cannot be found or do not fit.
function emptyList<T>(): T[] { return [] }
// expect-error: cannot infer type parameter 'T' of 'emptyList': nothing in the call says what it is; give it, e.g. 'emptyList<int>(...)'
const xs = emptyList()
function average<T extends number>(xs: T[]) { return xs.length }
// expect-error: type 'string' does not satisfy the constraint 'number' of 'T' in 'average'
average(["a"])
function pair<T>(a: T, b: T) { return [a, b] }
// expect-error: type parameter 'T' of 'pair' gets 'int' from one argument and 'string' from another
pair(1, "a")
function key<T extends string | int>(x: T) { return x }
// expect-error: type 'boolean' does not satisfy the constraint 'string | int' of 'T' in 'key'
key(true)
// expect-error: 'pair' takes 1 type argument, but got 2
pair<int, string>(1, 2)
function run<T>(f: (x: T) => void) { return 0 }
// expect-error: cannot infer type parameter 'T' of 'run' for parameter 'x' of this function: nothing else in the call says what it is
run(x => console.log(x))
function same<T>(x: T): T { return x }
// expect-error: type '(int) => int' is not assignable to type '(int) => string'
const toText: (x: int) => string = same
function plain(x: int) { return x }
// expect-error: 'plain' is not a generic function; remove the type arguments
plain<int>(1)

// A generic function as a value needs its types.
function first<T>(xs: T[]) { return xs[0] }
// expect-error: 'first' is generic, so a value of it needs its type arguments, e.g. 'first<int>', or a function type where it goes
const fs = [first]
// expect-error: 'first' is generic, so a value of it needs its type arguments
let later = first

// Recursion that needs ever new copies.
// expect-error: 'grow' calls itself with 'T[]' for 'T', so each copy of it for some type needs another for a bigger one, without end
function grow<T>(x: T, n: int): int { return n == 0 ? 0 : grow([x], n - 1) }
function ping<T>(x: T, n: int): int { return n == 0 ? 0 : pong([x], n - 1) }
function pong<T>(x: T, n: int): int { return ping(x, n) }
// expect-error: its type arguments keep growing
ping(1, 3)

// Declarations.
function host() {
  // expect-error: a generic function can be declared only at the top level for now
  function inner<T>(x: T) { return x }
  // expect-error: a generic arrow or function expression can only be the value of a top-level constant for now
  const local = <T>(x: T) => x
  return 0
}
// expect-error: 'int' is a built-in type and cannot name a type parameter
function shadow<int>(x: int) { return x }
// expect-error: duplicate type parameter 'T'
function twin<T, T>(x: T) { return x }
// expect-error: type parameter 'U' needs a default: it follows one that has one
function defaults<T = int, U>(x: U) { return x }

// Generic aliases.
type Box<T> = { value: T }
type Num<T extends number> = T[]
type Alias = int[]
// expect-error: 'Box' needs 1 type argument, e.g. 'Box<int>'
const b1: Box = { value: 1 }
// expect-error: 'Box' takes 1 type argument, but got 2
const b2: Box<int, int> = { value: 1 }
// expect-error: type 'string' does not satisfy the constraint 'number' of 'T' in 'Num'
const n1: Num<string> = []
// expect-error: type 'Alias' is not generic; remove the type arguments
const a1: Alias<int> = []
// expect-error: type alias 'Endless' refers to itself with other type arguments, so it never ends
type Endless<T> = { next?: Endless<T[]> }
