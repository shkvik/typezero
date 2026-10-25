// Operators and built-in functions used wrongly.
const x: int = 2
// expect-error: 'int' raised to a negative power is a fraction, which an integer cannot hold
const b = x ** -1
// expect-error: property 'toFixd' does not exist on type 'double'
const c = (1.5).toFixd(2)
// expect-error: argument of type 'string' is not assignable to parameter of type 'double'
const d = isNaN("x")
// expect-error: type 'int' is not assignable to type 'string'
const e = parseInt(5)
// expect-error: Number() takes a number, a boolean, a string or a decimal, found 'int[]'
const f = Number([1])
// expect-error: property 'parse' does not exist on 'Number'
const g = Number.parse("1")
// expect-error: expected 0-2 arguments, but got 3
const h = "a".split(",", 1, 2)
// expect-error: operator '-' cannot be applied to type 'string[]'
const i = -["5"]
// expect-error: expected 2 arguments, but got 1
const j = Math.atan2(1)
const parts = [1, 2, 3]
// expect-error: type 'string' is not assignable to type 'int'
parts.splice(0, 1, "x")
// expect-error: 'Math.PI' is a number, not a function
const k = Math.PI()
