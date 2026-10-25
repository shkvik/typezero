// JSON.parse<T> and fromJson<T> into unions and literal types
// (docs/narrowing.md, section 3): the kind of value JSON holds tells the
// members apart (typeof), a literal takes only its value, and objects go by
// a tag field, `kind: "circle"`. A value that fits no member is a TypeError
// with the path to it.
type Circle = { kind: "circle"; r: double }
type Square = { kind: "square"; side: double }
type Shape = Circle | Square
function area(s: Shape): double {
  return s.kind == "circle" ? 3 * s.r * s.r : s.side * s.side
}
const shapes = JSON.parse<Shape[]>('[{"kind": "circle", "r": 1}, {"side": 2, "kind": "square"}]')
console.log(shapes, shapes.map(area))

type Dir = "up" | "down"
const dirs = JSON.parse<Dir[]>('["up", "down", "up"]')
console.log(dirs, dirs[0] == "up")
type Level = 0 | 1 | 2
console.log(JSON.parse<Level>("2"), JSON.parse<true>("true"), fromJson<"on">(JSON.parse('"on"')))

// Members of different kinds.
type Value = string | int | boolean | int[]
console.log(JSON.parse<Value[]>('["a", 1, true, [1, 2]]'))
type Size = "auto" | int
console.log(JSON.parse<Size[]>('["auto", 12]'))
// Optional: a missing key and null give undefined.
type Options = { name: string; mode?: "fast" | "slow" }
console.log(JSON.parse<Options[]>('[{"name": "a"}, {"name": "b", "mode": "slow"}, {"name": "c", "mode": null}]'))

// Dictionaries: a Record with literal keys is an object with those fields;
// Record<string, T> inside a type is a dictionary.
type Grid = Record<"w" | "h", int>
console.log(JSON.parse<Grid>('{"w": 3, "h": 4}'))
type Inventory = { owner: string; counts: Record<string, int>; tags: Record<string, Dir[]> }
console.log(JSON.parse<Inventory>('{"owner": "x", "counts": {"a": 1, "b": 2}, "tags": {"t": ["up"]}}'))

function attempt(text: string) {
  try {
    console.log(JSON.parse<Shape>(text))
  } catch (e) {
    console.log(`${e}`)
  }
}
attempt('{"kind": "triangle", "a": 1}')
attempt('{"r": 1}')
attempt('{"kind": "circle", "r": "big"}')
attempt("5")
try {
  console.log(JSON.parse<Dir>('"left"'))
} catch (e) {
  console.log(`${e}`)
}
try {
  console.log(JSON.parse<Level[]>("[0, 3]"))
} catch (e) {
  console.log(`${e}`)
}
try {
  console.log(JSON.parse<Value>("null"))
} catch (e) {
  console.log(`${e}`)
}
try {
  console.log(JSON.parse<Inventory>('{"owner": "x", "counts": {}, "tags": {"t": ["up", "left"]}}'))
} catch (e) {
  console.log(`${e}`)
}
