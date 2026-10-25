// Recursion that runs out of stack throws a RangeError, as in JavaScript.
// expect-exit: 1
// expect-stderr: RangeError: Maximum call stack size exceeded
function grow(n: long): long {
  return n < 0 ? 0 : grow(n + 1) + grow(n + 2)
}
console.log("before")
console.log(grow(0))
