// expect-exit: 1
// expect-stderr: RangeError: Division by zero
// `**` of integers stays an integer, like `*`: it wraps around on overflow,
// and a negative power loses its fraction, like `/` (JavaScript would give
// 2147483648 and 0.5).
let two: int = 2
let thirtyOne: int = 31
let minusOne: int = -1
// expect-warning: 'two ** thirtyOne' may overflow int
console.log(two ** thirtyOne, two ** 30, two ** minusOne, (-two) ** minusOne, 1 ** minusOne)
let base: long = 2
console.log(base ** 62, base ** 63, base ** 64, (-base) ** 3)
let x: int = 5
x **= 2
console.log(x)
// Where the context wants a double, the literals are doubles.
const half: double = 2 ** -1
console.log(half)
// 0 to a negative power divides by zero, as `1 / 0` does for integers.
let zero: int = 0
for (let e = 1; e >= -1; e--) console.log(zero ** e)
