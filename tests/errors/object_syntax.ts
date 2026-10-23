// Object syntax that is not supported yet, or not at all.
const p = { x: 1, y: 2 }
// expect-error: methods in object literals are not supported yet; use a field with an arrow function: 'f: () => ...'
const withMethod = { f() { return 1 } }
// expect-error: getters and setters are not supported: reading or writing a field never runs code; write a function instead
const withGetter = { get x() { return 1 } }
// expect-error: spread in object literals is not supported yet; list the fields
const spread = { ...p, z: 3 }
// expect-error: a field name in brackets is not supported yet; write the name itself
const computed = { ["x"]: 1 }
// expect-error: a number cannot name a field; an object with numbered entries is an array
const numbered = { 1: "one" }
// expect-error: expected ':' after the field name, found '='
const defaulted = { x = 1 }
// expect-error: destructuring objects is not supported yet; read the fields one by one: 'const x = p.x'
const { x, y } = p
// expect-error: index signatures are not supported yet: an object has a fixed set of fields; a dictionary will be 'Record' or 'Map'
type Dictionary = { [key: string]: int }
// expect-error: methods in types are not supported yet; declare a field that holds a function, e.g. 'area: (x: int) => int'
interface Shape { area(): double }
// expect-error: field 'z' needs a type, e.g. 'z: int'
type Untyped = { z }
// expect-error: 'extends' for interfaces is not supported yet; list all the fields in 'Point3'
interface Point3 extends Point2 { z: int }
interface Point2 { x: int; y: int }
// expect-error: generic interfaces are not supported yet
interface Box<T> { value: int }
