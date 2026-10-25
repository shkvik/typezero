// same-as-node
// Numbers from strings, and numbers as text.
console.log(parseInt("42"), parseInt("  -17px"), parseInt("0x1F"), parseInt("ff", 16), parseInt("0b11"), parseInt("11", 2))
console.log(parseInt("z", 36), parseInt("12", 1), parseInt("12", 37), parseInt(""), parseInt("-"), parseInt("-0"), parseInt("3.9"))
console.log(parseInt("123456789012345678901234567890"), parseInt("0x"), parseInt("+8"), parseInt("  9"), parseInt("٣"))
console.log(parseFloat("3.14abc"), parseFloat(".5"), parseFloat("-.5e-3x"), parseFloat("1e"), parseFloat("1e+"), parseFloat("e5"))
console.log(parseFloat("Infinityx"), parseFloat("-Infinity"), parseFloat("0x10"), parseFloat("  2.5  "), parseFloat("."), parseFloat("-0"))
console.log(Number("42"), Number(" 4.5e1 "), Number(""), Number("  "), Number("0x1f"), Number("0b101"), Number("0o17"))
console.log(Number("12px"), Number("1_000"), Number("-0x10"), Number("Infinity"), Number("-Infinity"), Number("infinity"))
console.log(Number("+"), Number("-"), Number("+."), Number("e1"), Number("1e"), +"-", parseFloat("+"), parseInt("-"))
console.log(Number(".5"), Number("5."), Number("."), Number("+1e3"), Number(true), Number(false), Number(7), Number())
const nothing: int | undefined = undefined
const some: string | undefined = "8"
console.log(Number(nothing), Number(some), Number(undefined))

const zero: double = 0
console.log(isNaN(zero / zero), isNaN(1), isFinite(1 / zero), isFinite(2.5), isNaN(parseInt("x")))
console.log(Number.isNaN(zero / zero), Number.isFinite(5), Number.isInteger(5), Number.isInteger(5.5), Number.isInteger(1 / zero))
console.log(Number.isSafeInteger(2.0 ** 53), Number.isSafeInteger(2.0 ** 53 - 1), Number.isSafeInteger(3.5), Number.parseFloat("1.5"), Number.parseInt("08"))
console.log(Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER, Number.EPSILON, Number.MAX_VALUE, Number.MIN_VALUE)
console.log(Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NaN, NaN, Infinity, -Infinity)

// toFixed rounds the exact value of the double, halves away from zero.
const values = [0, 1.5, 2.5, -2.5, 1.005, 1.45, 0.000001, 123.456, -0.0001, 1e20, 1e21, 0.5, 8.345]
for (const v of values) console.log(v, v.toFixed(), v.toFixed(2), v.toFixed(5))
const third = 1.0 / 3
console.log(third.toFixed(20), (2.0 ** 60).toFixed(3), (5e-324).toFixed(100).length, (zero / zero).toFixed(2), (-1 / zero).toFixed(1))
const n: int = 7
console.log(n.toFixed(2), n.toString(), (-zero).toFixed(1))
