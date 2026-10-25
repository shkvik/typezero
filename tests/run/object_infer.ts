// same-as-node
// A literal without a type takes the types of its fields from where the
// object goes later: as if it were declared with the type of that place.
interface Point {
  x: double
  y: double
}
function len(p: Point) {
  return Math.hypot(p.x, p.y)
}
const p = { x: 3, y: 4 }
console.log(len(p), p.x / 2) // x is a double: 1.5, not 1

// Typed variables and fields, push, returns, assignments.
const q = { x: 1, y: 1 }
const typed: Point = q
const list: Point[] = []
const r = { x: 3, y: 3 }
list.push(r)
interface Line {
  from: Point
  to: Point
}
const a = { x: 1, y: 0 }
const line: Line = { from: a, to: { x: 1, y: 1 } }
function make(): Point {
  const s = { x: 1, y: 5 }
  return s
}
let w = { x: 7, y: 2 }
w = typed
console.log(typed.x / 2, list, r.x / 2, line, a.x / 2, make().y / 2, w.y / 2)

// Through closures and functions declared later, or in a block.
const later = { x: 5, y: 12 }
console.log(measure())
function measure() {
  return len(later) + later.x / 2
}
const outer = { x: 6, y: 0 }
{
  function quarter(pt: Point) {
    return pt.x / 4
  }
  console.log(quarter(outer), outer.x / 4)
}

// Arrays of literals, map's callbacks, the branches of ?:.
function total(ps: Point[]) {
  return ps.reduce((sum, pt) => sum + pt.x + pt.y, 0.0)
}
const pts = [
  { x: 1, y: 2 },
  { x: 3, y: 4 },
]
console.log(total(pts), pts[0]!.x / 2)
const moved: Point[] = pts.map(pt => ({ x: pt.x + 1, y: 0 }))
const xs = [1, 2, 3]
const made = xs.map(x => ({ x, y: x / 2 }))
console.log(moved, total(made), made)
const flag = xs.length > 2
const pick = flag ? { x: 7, y: 8 } : p
const other = flag ? p : { x: 9, y: 10 }
const mixed = [p, { x: 5, y: 6 }]
console.log(pick, pick.x / 2, other, mixed[1]!.x / 4)
function choose(left: boolean) {
  if (left) return p
  return { x: -1, y: -1 }
}
console.log(choose(false).x / 2)

// Optional fields the literal leaves out, and read-only ones it may change.
interface Node {
  value: int
  next?: Node
}
function lengthOf(node: Node | undefined): int {
  return node == undefined ? 0 : 1 + lengthOf(node.next)
}
const head = { value: 1 }
console.log(lengthOf(head), head)
interface Settings {
  readonly port: int
}
function show(s: Settings) {
  console.log(s.port)
}
const settings = { port: 80 }
settings.port = 8080
show(settings)
