// Integer arithmetic wraps around on overflow at run time (in constants it is
// an error, see errors/constants.ts).
let big: int = 2147483647
console.log(big + 1)
let small: int = -2147483648
console.log(small - 1)

// Integer division truncates; the remainder takes the sign of the dividend.
console.log(7 / 2, -7 / 2, 7 % 3, -7 % 3)
let min: int = -2147483648
let minusOne: int = -1
console.log(min / minusOne, min % minusOne)

// Literals take the type the context expects.
let half: double = 1 / 2
console.log(half)
let wide: long = 2147483647 + 1
console.log(wide)

// int widens to long and double on its own.
let n: int = 10
let l: long = 3000000000
console.log(n + l)
let x: double = n * 1.5
console.log(x)

// Bitwise operators; shift counts are masked like in JavaScript.
console.log(6 & 3, 6 | 3, 6 ^ 3, ~5)
console.log(1 << 4, -16 >> 2, -16 >>> 28, 1 << 33)

// Compound assignment, increment and decrement.
let c: int = 5
c += 3
c -= 1
c *= 2
c /= 3
c %= 3
console.log(c)
let d: double = 1
d /= 4
console.log(d)
let i: int = 0
console.log(i++, i, ++i, i--, --i)
