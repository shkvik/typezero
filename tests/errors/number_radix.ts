// A radix written as a literal is checked when compiling, as node would throw a RangeError.
const n = 255
// expect-error: toString() takes a radix from 2 to 36, found 40
console.log(n.toString(40))
// expect-error: toString() takes a radix from 2 to 36, found 1
console.log((1.5).toString(1))
