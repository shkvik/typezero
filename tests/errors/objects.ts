// Objects used wrongly: exact types, fields that do not exist or cannot be
// written, nothing from Object.prototype, and values that may be undefined.
interface Point {
  x: double
  y: double
}
function length(p: Point) {
  return Math.hypot(p.x, p.y)
}

// expect-error: type 'Point' has no field 'z'; an object must have exactly the fields of its type
const p1: Point = { x: 1, y: 2, z: 3 }
// expect-error: field 'y' is missing; type 'Point' requires it
const p2: Point = { x: 1 }
// expect-error: type 'string' is not assignable to type 'double' of field 'x'
const p3: Point = { x: "1", y: 2 }
const p: Point = { x: 1, y: 2 }
const p4 = { x: 1.0, y: 2.0, z: 3.0 }
// expect-error: an object must have exactly the fields of its type, and this one has the extra field 'z'
length(p4)
const p5 = { x: 1.0 }
// expect-error: type '{ x: double }' is not assignable to type 'Point'; it has no field 'y'
length(p5)
const p6 = { x: 1, y: 2 }
// expect-error: field 'x' is 'int' here and 'double' in 'Point': give the object its type where it is created, e.g. 'const p: Point = { ... }'
length(p6)
// expect-error: duplicate field 'x' in the object literal
const twice = { x: 1, x: 2 }
// expect-error: cannot infer the type of field 'none' from 'undefined'; declare the type of the object, e.g. 'const o: { none?: int } = ...'
const unknown = { none: undefined }
// expect-error: an expression of type 'void' cannot be the value of field 'nothing'
const nothing = { nothing: console.log() }
// expect-error: duplicate field 'a'
type Twice = { a: int; a: string }
// expect-error: 'void' can only be used as a return type
type Empty = { v: void }

// Fields that do not exist.
// expect-error: property 'z' does not exist on type 'Point'
console.log(p.z)
// expect-error: property 'z' does not exist on type 'Point'
p.z = 1
// expect-error: property 'toString' does not exist on type 'Point'; objects inherit nothing from Object.prototype
p.toString()
// expect-error: property 'hasOwnProperty' does not exist on type 'Point'; objects inherit nothing from Object.prototype
p.hasOwnProperty("x")
// expect-error: type 'Point' cannot be indexed; read a field by its name, e.g. 'o.name'
console.log(p["x"])
// expect-error: field 'x' is not a function; it has type 'double'
p.x()

// Read-only fields.
interface Settings {
  readonly port: int
}
const settings: Settings = { port: 80 }
// expect-error: cannot assign to 'port' because it is a read-only field of 'Settings'
settings.port = 8080
// expect-error: cannot assign to 'port' because it is a read-only field of 'Settings'
settings.port++
const open: { port: int } = { port: 1 }
const closed: Settings = open
// expect-error: field 'port' is read-only here, and '{ port: int }' would let it be changed
const reopened: { port: int } = closed

// Nothing from Object.prototype: no implicit conversion to a string.
// expect-error: an object cannot be converted to a string: JavaScript would give '[object Object]'; print it with console.log, or build the text from its fields
console.log(`${p}`)
// expect-error: an object cannot be converted to a string
console.log("point: " + p)
// expect-error: an object cannot be converted to a string
console.log(String(p))
// expect-error: objects cannot be converted to strings: JavaScript would give '[object Object]'; map them to text first, e.g. 'a.map(o => o.name).join()'
console.log([p].join())
// expect-error: sorting objects needs a comparison function: 'sort()' without one compares them all as '[object Object]'; pass one, e.g. 'sort((a, b) => a.x - b.x)'
;[p].sort()
// expect-error: operator '<' cannot be applied to types 'Point' and 'Point'
console.log(p < p)
// expect-error: operator '+' cannot be applied to types 'Point' and 'int'
console.log(p + 1)
const size = { width: 1, height: 2 }
// expect-error: operator '==' cannot be applied to types 'Point' and '{ width: int; height: int }'
console.log(p == size)

// Always truthy, and delete and in.
// expect-error: an object is always truthy, so the condition is always true; drop the check, or give it a type that allows undefined: 'Point | undefined'
if (p) console.log(p.x)
// expect-error: an object is always truthy, so '||' never gives its right side; drop '|| size'
const either = p || size
// expect-error: 'delete' cannot remove field 'x': an object always has the fields of its type
delete p.x
// expect-error: operator 'in' is not supported for objects: an object always has the fields of its type
console.log("x" in p)
// expect-error: cannot iterate over a value of type 'Point'
for (const v of p) console.log(v)
// expect-error: 'new Array<T>(n)' fills the array with a default value, and 'Point' has none; start from '[]' and use push()
const points = new Array<Point>(3)

// Values that may be undefined.
interface Node {
  value: int
  next?: Node
  onVisit?: () => void
}
const node: Node = { value: 1 }
// expect-error: the value may be undefined; check it before reading 'value'
console.log(node.next.value)
// expect-error: the value may be undefined; check it before assigning to 'value'
node.next.value = 2
// expect-error: the function may be undefined; check it first, or call it with '?.()', which gives undefined then
node.onVisit()
// expect-error: '??=' needs a field that may be undefined; 'value' has type 'int'
node.value ??= 2
// expect-error: 'null' is not supported: the only value for nothing is 'undefined'; write 'undefined'
node.next = null
// expect-error: 'null' is not supported: the only value for nothing is 'undefined'; write 'T | undefined'
let missing: Node | null

// A field narrowed by a check is forgotten when it may have changed.
function clear(n: Node) {
  n.next = undefined
}
function afterCall(n: Node, other: Node): int {
  if (n.next == undefined) return 0
  const first = n.next.value
  clear(other)
  // expect-error: the value may be undefined; check it before reading 'value'
  return first + n.next.value
}
function afterWrite(n: Node, other: Node): int {
  if (n.next == undefined) return 0
  other.next = undefined
  // expect-error: the value may be undefined; check it before reading 'value'
  return n.next.value
}
function afterLoop(n: Node): int {
  let total = 0
  if (n.next == undefined) return 0
  for (let i = 0; i < 3; i++) {
    // expect-error: the value may be undefined; check it before reading 'value'
    total += n.next.value
    n.next = n.next
  }
  return total
}

// An object cannot reach itself from its own initializer.
// expect-error: 'counter' is used in its own initializer, before it has a value
const counter = { n: 0, inc: () => counter.n++ }
const many: Point[] = [p]
// expect-error: type '{ x: int; y: int }[]' is not assignable to type 'Point[]'; field 'x' is 'int' here and 'double' in 'Point'
const ints: Point[] = many.map(q => ({ x: 1, y: 2 }))