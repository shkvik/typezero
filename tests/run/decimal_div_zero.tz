// A decimal divided by zero throws a RangeError, as `1n / 0n` does in
// JavaScript (BigNumber.js gives Infinity: a decimal has none).
// expect-exit: 1
// expect-stderr: RangeError: Division by zero
const prices: decimal[] = [10, 0]
let total: decimal = 0
for (const p of prices) {
  total += 1 / p
  console.log(total)
}
