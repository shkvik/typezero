// same-as-node
// Closures that share and change variables.
function makeAccount(start: int) {
  let balance = start
  const deposit = (n: int) => { balance += n; return balance }
  const withdraw = (n: int) => { balance -= n; return balance }
  return [deposit, withdraw]
}
const [dep, wd] = makeAccount(100)
console.log(dep!(50), wd!(30), dep!(5))

// Each iteration of `for (let ...)` has its own variable.
const later: (() => int)[] = []
for (let i = 0; i < 3; i++) {
  later.push(() => i * 10)
  i += 0
}
console.log(later.map(f => f()))

// A loop variable changed inside the body: the closure sees the change of
// its own iteration.
const seen: (() => int)[] = []
for (let i = 0; i < 6; i++) {
  i++
  seen.push(() => i)
}
console.log(seen.map(f => f()))

// Strings and arrays captured.
function greeter(greeting: string) {
  const names: string[] = []
  return (name: string) => {
    names.push(name)
    return greeting + ", " + names.join(" and ")
  }
}
const hello = greeter("Hello")
console.log(hello("Ann"))
console.log(hello("Bob"))

// Functions declared in blocks: hoisted, recursive, calling each other.
function parity(n: int): string {
  function isEven(k: int): boolean { return k === 0 ? true : isOdd(k - 1) }
  function isOdd(k: int): boolean { return k === 0 ? false : isEven(k - 1) }
  return isEven(n) ? "even" : "odd"
}
console.log(parity(10), parity(7))

// A nested function that uses a variable declared after it.
function scaled(xs: int[]) {
  const factor = 3
  function scale(x: int) { return x * factor + offset }
  let offset = 0
  offset = 1
  const result = xs.map(scale)
  offset = 100
  return result.concat(xs.map(scale))
}
console.log(scaled([1, 2]))

function counterBlock() {
  let count = 0
  function bump() { count++ }
  bump()
  bump()
  return count
}
console.log(counterBlock())

// A constant arrow calls itself; so does a named function expression.
const fact = (n: int): int => n <= 1 ? 1 : n * fact(n - 1)
const fib = function f(n: int): int { return n < 2 ? n : f(n - 1) + f(n - 2) }
console.log(fact(10), fib(15))
const sumTo = (n: int) => n === 0 ? 0 : n + sumTo(n - 1)
console.log(sumTo(100))

// Functions as values: returned, stored, passed, compared.
function compose(f: (x: int) => int, g: (x: int) => int) {
  return (x: int) => f(g(x))
}
const inc = (x: int) => x + 1
const dbl = (x: int) => x * 2
console.log(compose(inc, dbl)(5), compose(dbl, inc)(5))
const adder = (a: int) => (b: int) => a + b
console.log(adder(3)(4))
function apply(f: (x: int) => int, x: int) { return f(x) }
function square(x: int) { return x * x }
console.log(apply(square, 7), apply(x => -x, 7))
const ops: ((x: int) => int)[] = [inc, dbl, square]
console.log(ops.map(op => op(6)))
console.log(inc === inc, inc === dbl, typeof inc, ops.indexOf(dbl))
console.log(ops)

// Optional functions and `?.()`.
function run(task?: () => string) {
  return task?.() ?? "nothing"
}
console.log(run(), run(() => "done"))

// An immediately invoked function.
const answer = ((x: int) => x * 6)(7)
console.log(answer);
(function () { console.log("iife") })()

// Default values and functions with fewer parameters.
function each(xs: int[], visit: (x: int, i: int) => void = (x, i) => console.log(i, x)) {
  for (let i = 0; i < xs.length; i++) visit(xs[i], i)
}
each([7, 8])
each([9], x => console.log("value", x))
