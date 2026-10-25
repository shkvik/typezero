// Spreads, destructuring, keys in brackets, Object.keys, for...in and tuples
// used wrongly.
interface Point {
  x: double
  y: double
}
interface IntPoint {
  x: int
  y: int
}
function len(p: Point) {
  return Math.hypot(p.x, p.y)
}
function sum(p: IntPoint) {
  return p.x + p.y
}
// One object cannot have a field of two types.
const p = { x: 1, y: 2 }
// expect-error: field 'x' is 'int' here and 'double' in 'Point'; the same object also goes where 'IntPoint' is expected, and one object cannot be both
len(p)
sum(p)

// A field that is not there is undefined: `in` and `delete` say nothing new.
interface Query {
  text: string
  limit?: int
}
const query: Query = { text: "cats" }
// expect-error: operator 'in' is not supported for objects: a field that is not there is undefined; check it with 'query.limit != undefined'
console.log("limit" in query)
// expect-error: operator 'in' is not supported for objects: an object always has the fields of its type, so this would always be true
console.log("text" in query)
// expect-error: type 'Query' has no field 'other', so this would always be false
console.log("other" in query)
// expect-error: 'delete' cannot remove field 'limit': a field that is not there is undefined; write 'query.limit = undefined'
delete query.limit
// expect-error: write 'query.limit = undefined'
delete query["limit"]
// expect-error: 'delete' cannot remove field 'text': an object always has the fields of its type
delete query.text

// for...in gives the indices of an array as strings.
const xs = [10, 20]
// expect-error: 'for...in' over an array gives its indices as strings ('0', '1'...), so 'i + 1' would be '01'; walk the elements with 'for (const x of xs)', or the indices with 'for (let i = 0; i < xs.length; i++)' or 'xs.forEach((x, i) => ...)'
for (const i in xs) console.log(i + 1)
// expect-error: 'for...in' over a string gives its indices as strings
for (const c in "abc") console.log(c)
// expect-error: 'for...in' walks the keys of an object, found 'int'
for (const k in 5) console.log(k)

// Object.values and entries put all the values in one array, of the union of
// their types.
const mixed = { a: 1, b: "x" }
const lists = { a: [1], b: ["x"] }
// expect-error: 'Object.values' puts the values of all the fields in one array, of the union of their types, and the fields of '{ a: int[]; b: string[] }' have types that no union holds: 'int[]' and 'string[]' cannot be told apart when the program runs
console.log(Object.values(lists))
// expect-error: 'Object.entries' puts the values of all the fields in one array
console.log(Object.entries(lists))
// expect-error: 'Object.keys' needs an object, found 'int[]'
console.log(Object.keys(xs))
// expect-error: 'Object.assign' is not supported; only Object.keys, Object.values, Object.entries, Object.fromEntries and Object.groupBy are
console.log(Object.assign({}, mixed))
function find(text: string): Query | undefined {
  return text == query.text ? query : undefined
}
const maybe = find("cats")
// expect-error: the object may be undefined; check it first
console.log(Object.keys(maybe))

// Keys in brackets must be known when compiling.
let name = "a"
// expect-error: type '{ a: int; b: string }' can be indexed only by a key known when compiling: a string literal, or a constant declared with one, such as 'const key = "name"'; keys computed when the program runs need a dictionary: declare it as one, e.g. 'const d: Record<string, int> = {}'
console.log(mixed[name])
// expect-error: a key in brackets must be known when compiling
const dynamic = { [name]: 1 }
// expect-error: type '{ a: int; b: string }' has no numbered fields; read a field by its name, e.g. 'o.name'
console.log(mixed[0])
// expect-error: a number cannot name a field; an object with numbered entries is an array
const numbered = { [1]: "one" }
// expect-error: property 'c' does not exist on type '{ a: int; b: string }'
console.log(mixed["c"])
// expect-error: a key must be a string, found 'boolean'
const flagged = { [true]: 1 }

// Spreads and patterns.
// expect-error: only objects can be spread into an object literal, found 'int[]'; JavaScript would make its indices the keys
const indices = { ...xs }
// expect-error: the object may be undefined; check it before spreading it
const fromMaybe = { ...maybe }
const words = { a: "x" }
const partial: { a?: int } = { a: 1 }
// expect-error: field 'a' is 'string' before the spread and 'int | undefined' in '{ a?: int }'; a field has one type
const both = { ...words, ...partial }
// expect-error: 'x = ...' gives a default value, which only a destructuring pattern such as '({ x = 1 } = o)' has; in an object literal write 'x: ...'
const defaulted = { x = 1 }
// expect-error: property 'z' does not exist on type 'Point'
const { z } = { x: 1.0, y: 2.0 }
// expect-error: the value may be undefined; check it before reading 'text'
const { text } = maybe
// expect-error: destructuring assignment can only be a statement, e.g. '({ a, b } = o)'
const assigned = ({ x: name } = { x: "b" })
// expect-error: parameter '{ x, y }' needs a type annotation
function untyped({ x, y }) {}

// Tuples have a fixed length and a type for each place.
const t: [string, int] = ["a", 1]
// expect-error: a tuple has a fixed length, and 'push' would change it; to change the length, copy it into an array: '[...t]'
t.push(2)
// expect-error: 'indexOf' is not supported on tuples; copy the tuple into an array first: '[...t].indexOf(...)'
t.indexOf("a")
let j = 1
// expect-error: a tuple can be indexed only by a constant, such as '[0]': each element has its own type; or destructure it: 'const [a, b] = t'
console.log(t[j])
// expect-error: tuple '[string, int]' has 2 elements; there is no element 2
console.log(t[2])
// expect-error: type '[string, int]' is not assignable to type 'string[]'; a tuple is not an array, though it is written like one: copy it into an array with '[...t]'
const arr: string[] = t
// expect-error: cannot iterate over a tuple, whose elements have types of their own; destructure it, or copy it into an array: '[...t]'
for (const v of t) console.log(v)
// expect-error: a tuple cannot be converted to a string; print it with console.log, or build the text from its elements
console.log(`${t}`)
// expect-error: type '[string, int]' has 2 elements, and this literal has 3
const long: [string, int] = ["a", 1, 2]
// expect-error: type 'string' is not assignable to type 'int' of element 1
const wrong: [string, int] = ["a", "b"]
// expect-error: a tuple has a fixed length, which cannot be changed
t.length = 3
const lt: [int[], string[]] = [[1], ["a"]]
// expect-error: the elements of '[int[], string[]]' have different types, and an array holds elements of one type
const spread = [...lt]
// expect-error: a tuple cannot take spread values: type '[string, int]' has a fixed length; write each element
const fromSpread: [string, int] = [...t]
const fixed: readonly [int, int] = [1, 2]
// expect-error: cannot assign to '0' because it is a read-only field of 'readonly [int, int]'
fixed[0] = 3
// expect-error: array elements must have one type; found 'int[]' and 'string[]'; for a value of each type in its own place, declare a tuple, e.g. 'const t: [int[], string[]] = ...'
const pair = [[1], ["a"]]
