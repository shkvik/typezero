// same-as-node
// Lists of the types of parameters (docs/generics.md, step 3): `A extends
// unknown[]` is what a function takes, `...args: A` passes it on, and
// `[X, ...A]`, `[...A, X]`, `[...A, ...B]` take a part of it. In a copy the
// list is plain parameters: the wrapper of add is `(a0: int, a1: int)`.
function add(a: int, b: int) { return a + b }
function logged<A extends unknown[], R>(name: string, fn: (...args: A) => R) {
  return (...args: A): R => {
    console.log("->", name, ...args)
    return fn(...args)
  }
}
const loggedAdd = logged("add", add)
console.log(loggedAdd(1, 2))

// Optional and rest parameters stay so. (A parameter left out is undefined
// in the copy's list: printed, it would show, where JavaScript's `args` is
// shorter.)
function greet(name: string, greeting?: string) { return `${greeting ?? "hi"}, ${name}` }
function named<A extends unknown[], R>(name: string, fn: (...args: A) => R) {
  return (...args: A): R => {
    console.log("->", name)
    return fn(...args)
  }
}
const loggedGreet = named("greet", greet)
console.log(loggedGreet("ann"), loggedGreet("bob", "yo"))
function sum(...xs: int[]) {
  let s = 0
  for (const x of xs) s += x
  return s
}
const loggedSum = logged("sum", sum)
console.log(loggedSum(), loggedSum(1, 2, 3))
const loggedArrow = logged("arrow", (s: string, n: int) => s.repeat(n))
console.log(loggedArrow("ab", 2))

// Taking a part off the list, or adding one.
function fmt(prefix: string, n: int, flag: boolean) { return `${prefix}${n}${flag}` }
function bindFirst<X, A extends unknown[], R>(fn: (x: X, ...rest: A) => R, x: X) {
  return (...rest: A): R => fn(x, ...rest)
}
console.log(bindFirst(fmt, "#")(1, true))
function bindLast<A extends unknown[], X, R>(fn: (...args: [...A, X]) => R, x: X) {
  return (...args: A): R => fn(...args, x)
}
console.log(bindLast(fmt, false)("q", 3))
function labelled<A extends unknown[], R>(fn: (...args: A) => R) {
  return (label: string, ...args: A): R => {
    console.log(label)
    return fn(...args)
  }
}
console.log(labelled(fmt)("call", "p", 2, false))

// Partial application: the list in two parts, the first given.
function add3(a: int, b: int, c: int) { return a + b + c }
function partial<A extends unknown[], B extends unknown[], R>(fn: (...args: [...A, ...B]) => R, ...head: A) {
  return (...tail: B): R => fn(...head, ...tail)
}
const plus3 = partial(add3, 1, 2)
const plus = partial(add3, 100)
console.log(plus3(10), plus(1, 2), partial(add3)(1, 1, 1))

// What can be done with `args`.
function call<A extends unknown[], R>(fn: (...args: A) => R, ...args: A) { return fn(...args) }
console.log(call(fmt, "z", 9, true), call(add, 4, 5))
function count<A extends unknown[]>(...args: A) { return args.length }
console.log(count(1, "a", true), count())
function first<A extends unknown[]>(...args: A) { return args[0] }
console.log(first(5, "a"), first("s").length)
function show<A extends unknown[]>(...args: A) {
  const parts: string[] = []
  for (const x of args) parts.push(`${x}`)
  return parts.join("|")
}
console.log(show(1, "a", 2.5, true))
function keep<A extends unknown[]>(...args: A) { return args }
const kept = keep(1, "b")
console.log(kept, kept[1].length)

// An array spread into console.log gives its elements as arguments.
const nums = [1, 2]
const none: string[] = []
console.log(...nums, "x", ...none, ...["y", "z"])
console.log(...none)
