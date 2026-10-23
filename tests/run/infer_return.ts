// same-as-node
// Without an annotation a function returns what its `return` statements give.
function area(w: int, h: int) {
  return w * h
}
function half(x: int) {
  return x / 2.0
}
function describe(n: int) {
  if (n < 0) return "negative"
  return n == 0 ? "zero" : "positive"
}
function firstWord(s: string) {
  const space = s.indexOf(" ")
  if (space < 0) return undefined
  return s.slice(0, space)
}
function mixed(flag: boolean) {
  if (flag) return 1 // an int and a double: a double
  return 2.5
}
function positive(n: int) {
  if (n > 0) return n // may also end without a value: int | undefined
}
console.log(area(3, 4), half(5), describe(-1), describe(0), describe(7))
console.log(firstWord("hello world"), firstWord("single"), mixed(true), mixed(false), positive(3), positive(-3))
const square: int = area(5, 5) // an int, so it fits an int variable
console.log(square)

// Recursion, also mutual: the type settles from the cases that do not
// recurse, and widens where needed (power is a double).
function fib(n: int) {
  return n < 2 ? n : fib(n - 1) + fib(n - 2)
}
function fact(n: int) {
  if (n <= 1) return 1
  return n * fact(n - 1)
}
function power(x: double, n: int) {
  if (n == 0) return 1
  return x * power(x, n - 1)
}
function repeat(s: string, n: int) {
  return n <= 0 ? "" : s + repeat(s, n - 1)
}
function isEven(n: int) {
  return n == 0 ? true : isOdd(n - 1)
}
function isOdd(n: int) {
  return n == 0 ? false : isEven(n - 1)
}
function depth(tree: int[], at: int) {
  if (at >= tree.length) return 0
  return 1 + Math.max(depth(tree, 2 * at + 1), depth(tree, 2 * at + 2))
}
console.log(fib(20), fact(10), power(1.5, 3), repeat("ab", 3), isEven(10), isOdd(7), depth([1, 2, 3, 4, 5], 0))

// A function is visible before its declaration, its return type too.
console.log(later(2))
function later(x: int) {
  return [x, x * 2]
}

// Without a returned value a function is void.
function log(message: string) {
  console.log("log:", message)
}
log("done")
