// Object syntax that is not supported yet, or not at all.
const p = { x: 1, y: 2 }
// expect-error: a method cannot have a key in brackets; write its name: 'f() { ... }'
const computedMethod = { ["f"]() { return 1 } }
// expect-error: getters and setters are not supported: reading or writing a field never runs code; write a function instead
const withGetter = { get x() { return 1 } }
// expect-error: a number cannot name a field; an object with numbered entries is an array
const numbered = { 1: "one" }
// expect-error: an index signature cannot stand next to fields
type Mixed = { [key: string]: int; size: int }
// expect-error: method 'area' needs its return type, e.g. 'area(): void'
interface Shape { area() }
// expect-error: field 'z' needs a type, e.g. 'z: int'
type Untyped = { z }

// Destructuring patterns.
// expect-error: a destructuring declaration needs a value: '{ a, b } = ...'
const { x }
// expect-error: expected a variable name in the destructuring pattern, found integer literal
const { y: 1 } = p
// expect-error: expected a variable name after '...' in the destructuring pattern
const { ...{ a } } = p
// expect-error: a key in a destructuring pattern needs a variable: 'a-b: name'
const { "a-b" } = p
// expect-error: a rest element must be the last one
const { ...first, y } = p
// expect-error: expected a parameter name after '...'
function rest(...{ length }: int[]) {}
// expect-error: the variable of 'for...in' is a key, a string, and cannot be destructured
for (const [a, b] in p) console.log(a, b)
let k = ""
// expect-error: 'for...in' needs a new variable: 'for (const k in ...)'
for (k in p) console.log(k)

// Tuple types.
// expect-error: optional elements in a tuple type are not supported: a tuple has a fixed length; give the element the type 'T | undefined'
let optional: [int, string?] = [1, "a"]
