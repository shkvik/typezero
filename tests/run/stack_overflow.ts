// Recursion that runs out of stack stops the program, as JavaScript throws a
// RangeError.
// expect-exit: 2
// expect-stderr: RangeError: Maximum call stack size exceeded
function grow(n: long): long {
  return n < 0 ? 0 : grow(n + 1) + grow(n + 2)
}
console.log("before")
console.log(grow(0))
