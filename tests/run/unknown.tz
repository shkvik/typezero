// same-as-node
// unknown holds a value of any type; a check shows what it holds.

class Point {
  x: double
  y: double
  constructor(x: double, y: double) {
    this.x = x
    this.y = y
  }
  length() {
    return Math.hypot(this.x, this.y)
  }
}

function describe(v: unknown): string {
  if (v == undefined) return "nothing"
  if (typeof v == "string") return `a string of ${v.length}: ${v.toUpperCase()}`
  if (typeof v == "number") return `a number, twice ${v * 2}`
  if (typeof v == "boolean") return v ? "yes" : "no"
  if (v instanceof Point) return `a point ${v.length()} away`
  if (Array.isArray(v)) return `an array of ${v.length}`
  if (typeof v == "object") return "some object"
  return `a ${typeof v}`
}

const values: unknown[] = ["hello", 21, 2.5, true, false, undefined, new Point(3, 4), [1, 2, 3], { a: 1 }]
for (const v of values) console.log(describe(v))
console.log(describe(describe))

// Printing shows what it holds, as node does.
console.log(values)
const one: unknown = "text"
console.log(one, typeof one)
let maybe: unknown
console.log(maybe, typeof maybe)
maybe = 5
console.log(maybe, typeof maybe)

// Equality is `===`.
const a: unknown = 1
const b: unknown = 1.0
const c: unknown = "1"
console.log(a == b, a == undefined, maybe == 5, c == "1")
const p = new Point(1, 1)
const q: unknown = p
console.log(q == p, q == new Point(1, 1))

// `??` replaces undefined.
let none: unknown = undefined
console.log(none ?? "default", a ?? "default")

// The elements of an array that a check showed are unknowns themselves.
const nested: unknown = [[1, "a"], [true]]
if (Array.isArray(nested)) {
  for (const row of nested) {
    if (Array.isArray(row)) console.log(row.length, row[0])
  }
}

// A Record of unknowns, as libraries keep settings of any type.
const settings: Record<string, unknown> = { port: 8080, host: "localhost", debug: true }
const port = settings["port"]
if (typeof port == "number") console.log(port + 1)
const host = settings.host
if (typeof host == "string") console.log(host.length)
console.log(settings)

// object is any object of the language.
function className(o: object): string {
  if (o instanceof Point) return "Point"
  return "other"
}
console.log(className(p), className({ x: 1 }), className(new Date(0)))
const objects: object[] = [p, { y: 2 }]
console.log(objects)

// JSON.stringify writes what it holds.
console.log(JSON.stringify(values), JSON.stringify(maybe), JSON.stringify(none))
console.log(JSON.stringify(settings, null, 2))

// Searching an unknown[] compares what the elements hold, as '===' does.
const mixed: unknown[] = [1, "a", true, undefined]
console.log(mixed.includes(1), mixed.indexOf("a"))
console.log(mixed.includes(undefined), mixed.lastIndexOf(true))
console.log(mixed.filter(x => typeof x == "string"), mixed.map(x => typeof x))
console.log(mixed.length, mixed.at(-1), mixed.slice(1, 2))
const picked: unknown = 2
switch (picked) {
  case 1:
    console.log("one")
    break
  case 2:
    console.log("two")
    break
  default:
    console.log("other")
}
