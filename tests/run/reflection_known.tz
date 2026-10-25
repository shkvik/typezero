// A class known when compiling, given where a generic function takes
// `Constructor<T>`: its copy for that class takes the class itself, so
// `new cls(...)`, `x instanceof cls` and `fromJson(cls, j)` in it are those
// of the class, checked when compiling, without reflection
// (docs/decorators.md, plan step 5). A class known only when the program
// runs goes through its description, as before.

class Point {
  constructor(public x: int, public y: int) {}
  norm() {
    return Math.abs(this.x) + Math.abs(this.y)
  }
}
class Pair {
  constructor(public left: int, public right: int) {}
}

function make<T extends object>(cls: Constructor<T>, a: int, b: int): T {
  return new cls(a, b)
}
function count<T extends object>(xs: object[], cls: Constructor<T>) {
  let n = 0
  for (const x of xs)
    if (x instanceof cls)
      n++
  return `${cls.name}: ${n}`
}
function load<T extends object>(cls: Constructor<T>, text: string): T {
  return fromJson(cls, JSON.parse(text))
}

const p = make(Point, 3, -4)
console.log(p, p.norm())
const things: object[] = [p, new Pair(1, 2), make(Pair, 5, 6), new Point(0, 0)]
console.log(count(things, Point), count(things, Pair))
console.log(load(Point, '{"x": 1, "y": 2}').norm())
// The same functions with classes known only when the program runs.
const kinds: Constructor[] = [Point, Pair]
for (const kind of kinds) {
  console.log(make(kind, 7, 8), count(things, kind), load(kind, '{"x": 1, "y": 2, "left": 3, "right": 4}'))
}
