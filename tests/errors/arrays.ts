// expect-error: cannot infer the element type of an empty array
const empty = []
// expect-error: array elements must have one type; found 'int' and 'string'
const mixed = [1, "x"]
const ints: int[] = [1, 2]
// expect-error: type 'int[]' is not assignable to type 'double[]'
const doubles: double[] = ints
// expect-error: 'map' takes a callback, and arrow functions are not supported yet
const twice = ints.map(2)
// expect-error: 'new Array<T>(n)' fills the array with a default value, and 'int[]' has none
const rows = new Array<int[]>(3)
// expect-error: an array length cannot be negative
const negative = new Array<int>(-1)
// expect-error: 'new Array' needs an element type
const untyped = new Array(3)
// expect-error: a 'for...of' variable cannot have a type annotation
for (const v: int of ints) {}
// expect-error: cannot iterate over a value of type 'int'
for (const v of 5) {}
const text = "abc"
// expect-error: strings are immutable
text[0] = "x"
// expect-error: cannot spread 'int[]' into 'double[]'
const spread: double[] = [...ints]
// expect-error: property 'size' does not exist on type 'int[]'
const size = ints.size
// expect-error: a comparison function needs arrow functions
ints.sort(1)
// expect-error: union types other than 'T | undefined' are not supported yet
let either: int | string = 1
