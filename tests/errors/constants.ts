// Integer constants that would wrap around, where JavaScript keeps the value.
// expect-error: this constant is 1099511627776, which does not fit in 'int' and would wrap around to 0
const a = 2 ** 40
const MS_PER_DAY = 24 * 60 * 60 * 1000
// expect-error: this constant is 2592000000, which does not fit in 'int' and would wrap around to -1702967296; compute it as a 'long' or a 'double'
const month = MS_PER_DAY * 30
// expect-error: this constant is 2147483648, which does not fit in 'int'
const b = 2147483647 + 1
const min = -2147483648
// expect-error: this constant is 2147483648
const c = -min
// expect-error: this constant is 2147483648
const d = min / -1
// expect-error: this constant is -2147483649
const e = min - 1
// expect-error: this constant is 9223372036854775808, which does not fit in 'long' and would wrap around to -9223372036854775808; compute it as a 'double'
const f: long = 9223372036854775807 + 1
// expect-error: this constant is 9007199254740992, which does not fit in 'int'
console.log(Number.isSafeInteger(2 ** 53))
enum Size {
  Large = 1000000,
}
// expect-error: this constant is 1000000000000
console.log(Size.Large * Size.Large)

// Under `| 0` only the low bits count, but JavaScript rounds beyond 2^53.
// expect-error: beyond 2^53, where JavaScript rounds it before taking its low bits; for a 32-bit product use Math.imul
const g = (2147483647 * 2147483647) | 0

// One error for the first operation that overflows, not for each around it.
// expect-error: this constant is 4294967296
const h = (65536 * 65536 + 1) * 2

// Integer division by a constant zero always panics.
let x = 5
// expect-error: integer division by zero; to get Infinity or NaN as in JavaScript, divide doubles
console.log(x / 0)
// expect-error: integer division by zero
console.log(x % (1 - 1))
// expect-error: integer division by zero
x /= 0

// A negative constant exponent, not only a literal one.
const NEG = -1
// expect-error: 'int' raised to a negative power is a fraction
console.log(2 ** NEG)
