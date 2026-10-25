// same-as-node
// Converting numbers works as in JavaScript: there is no `as`.

// `x | 0` and the other bitwise operators turn a double into an int the way
// JavaScript does: the fraction is dropped, the rest wraps around modulo 2^32,
// and NaN and the infinities become 0.
let f: double = 1.9
let zero: double = 0
console.log(f | 0, -f | 0, (0.5 - 1) | 0, (zero / zero) | 0, (1 / zero) | 0, (-1 / zero) | 0)
console.log(2147483647.5 | 0, 2147483648.5 | 0, 4294967296.5 | 0, -2147483649.5 | 0)
console.log(1e10 | 0, -1e10 | 0, 1e300 | 0, 5e-324 | 0)
const n: int = f * 10 | 0
console.log(n, ~f, ~~(-f), f & 1, 7.9 ^ 2.1, 2.5 << 3, -9.5 >> 1)
let x = 123.75
x |= 0
const ds: double[] = [1.5, -2.7]
ds[0] |= 0
ds[1] <<= 1
let k = 6
k |= 1.5
k <<= 1.9
console.log(x, ds, k)

// Math functions return doubles, and integers where an int or a long is
// expected.
const values: double[] = [2.5, -2.5, 0.5, -0.5, -0.4, 0.49999999999999994, 1.5, -0, 1e21]
for (const v of values)
  console.log(v, Math.floor(v), Math.ceil(v), Math.round(v), Math.trunc(v))
console.log(Math.round(zero / zero), Math.floor(1 / zero), Math.ceil(-1 / zero))
const rounded: int = Math.round(1234.5)
const floored: long = Math.floor(-1e15 - 0.5)
const cents: int = Math.round(19.99 * 100)
console.log(rounded, floored, cents, rounded + 1)
const count: int = 7
console.log(Math.floor(count), Math.round(count))
const big: long = 5000000000
const small: int = Math.trunc(big - 4000000000)
console.log(small)

// A long becomes a double by itself, as in C#; beyond 2^53 it rounds, as every
// number does in JavaScript.
const huge: long = 9007199254740993
const asDouble: double = huge
console.log(asDouble, huge + 0.5, huge * 2.0 > 1e16)
