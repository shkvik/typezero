// JavaScript's `x >>> n` is unsigned: for an int and a count that may be 0
// the result is a long (`-1 >>> 0` is 4294967295), which an int cannot hold.
let h = -5
let k = 3
// expect-error: type 'long' is not assignable to type 'int'; '>>>' is unsigned in JavaScript
// expect-error: keep the result in a long, e.g. 'const u = h >>> 0'
h = h >>> 0
// expect-error: type 'long' is not assignable to type 'int'; '>>>' is unsigned
const v: int = h >>> k
// expect-error: the result of '>>>=' may not fit in 'int'; '>>>' is unsigned in JavaScript
// expect-error: e.g. 'const u = h >>> k', or shift by 1 to 31 bits
h >>>= k
// expect-error: the result of '>>>=' may not fit in 'int'
h >>>= 32
// A count of 1 to 31 keeps an int, and so does `| 0`.
h >>>= 1
h = (h >>> k) | 0
console.log(h, v)
