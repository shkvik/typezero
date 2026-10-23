// Destructuring, rest parameters, spread, `||=`, `?.` and `!` used wrongly.
// expect-error: only arrays can be destructured, found 'int'
const [a, b] = 5
function maybe(): int[] | undefined {
  return undefined
}
// expect-error: only arrays can be destructured, found 'int[] | undefined'; the array may be undefined: check it first
const [c] = maybe()
// expect-error: only arrays can be destructured; the elements are 'int'
for (const [d] of [1, 2]) console.log(d)
let x = 1
let y = 2
// expect-error: destructuring assignment can only be a statement, e.g. '[a, b] = [b, a]'
const z = ([x, y] = [y, x])
// expect-error: arrays cannot have holes; write the element explicitly
const holes = [1, , 2]

// expect-error: a rest parameter must be an array, e.g. '...xs: int[]'
function bad(...xs: int): void {}
function two(a: int, b: int): int {
  return a + b
}
const nums = [1, 2]
// expect-error: a spread argument can only be passed to a rest parameter, Math.min, Math.max or push
two(...nums)
function some(first: int, ...rest: int[]): void {}
// expect-error: a spread argument can only be passed to a rest parameter
some(...nums)
// expect-error: a spread argument can only be passed to a rest parameter
console.log(...nums)
// expect-error: a spread argument can only be passed to a rest parameter
Math.sqrt(...nums)
// expect-error: a spread argument can only be passed to a rest parameter
nums.unshift(...nums)
// expect-error: only an array of numbers can be spread here, found 'string[]'
Math.max(..."ab".split(""))
// expect-error: Math.max of an array that may be empty is -Infinity, which only a double holds; check that the array is not empty, or pass a first value: 'Math.max(0, ...a)'
const best: int = Math.max(...nums.slice(1))
const fine: int = Math.max(...nums) // nums is not empty

let count = 0
// expect-error: '||=' cannot test a number: 0 would count as false; compare it: 'if (count == 0) count = 5'
count ||= 5
const flags = [true]
function index(): int {
  return 0
}
// expect-error: '&&=' needs a variable, a field or an array element at a simple index, such as 'a[i]'
flags[index()] &&= false

// expect-error: the value is always undefined
const u = undefined!
