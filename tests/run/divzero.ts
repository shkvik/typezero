// expect-exit: 1
// expect-stderr: RangeError: Division by zero
// Division by zero is checked at run time, also where the divisor is only
// known to be zero after inlining and unrolling.
function divide(a: int, b: int): int {
  return a / b
}
for (let i = 3; i >= 0; i--) console.log("10 /", i, "=", divide(10, i))
console.log("after")
