// What reflection checks when the program runs (docs/decorators.md, section
// 6), where JavaScript goes on with what it got: each argument against the
// parameter it goes to, how many there are, the `this` of a method; a value
// that does not fit is a TypeError that says which and why.

class Point {
  constructor(public x: int, public y: int) {}
  move(dx: int, dy?: int) {
    this.x += dx
    this.y += dy ?? 0
    return this
  }
  scale(...factors: double[]) {
    let k = 1.0
    for (const f of factors) k *= f
    return `${this.x * k}, ${this.y * k}`
  }
}
class Label {
  constructor(public text: string, public at: Point) {}
}
abstract class Shape {
  abstract area(): double
}
class Square extends Shape {
  constructor(public side: double) {
    super()
  }
  area() {
    return this.side * this.side
  }
}

function attempt(what: string, run: () => unknown) {
  try {
    console.log(what, run())
  } catch (e) {
    console.log(what, String(e))
  }
}

const kind: Constructor = Point
const labelKind: Constructor<Label> = Label
const p = new Point(1, 2)
attempt("new", () => new kind(3, 4))
attempt("new", () => new kind(1, 2.5))
attempt("new", () => new kind(1))
attempt("new", () => new kind(1, 2, 3))
attempt("new", () => new kind("1", 2))
attempt("new", () => new labelKind("here", p))
attempt("new", () => new labelKind("here", new Label("x", new Point(0, 0))))
const move = Reflect.get(p, "move")
attempt("apply", () => Reflect.apply(move, p, [3]))
attempt("apply", () => Reflect.apply(move, p, [3, 4]))
attempt("apply", () => Reflect.apply(move, p, []))
attempt("apply", () => Reflect.apply(move, p, [true]))
attempt("apply", () => Reflect.apply(move, new Label("l", p), [1]))
attempt("apply", () => Reflect.apply(Reflect.get(p, "scale"), p, [2, 0.5, 3]))
attempt("apply", () => Reflect.apply(Reflect.get(p, "scale"), p, [2, "x"]))
const five: unknown = 5
attempt("apply", () => Reflect.apply(five, p, []))
attempt("apply", () => Reflect.apply(Reflect.get(p, "x"), p, []))
const shapes: Constructor<Shape>[] = [Square, Shape]
attempt("construct", () => Reflect.construct(shapes[0]!, [3]).area())
attempt("construct", () => Reflect.construct(shapes[1]!, []))
attempt("get", () => Reflect.get(p, "x"))
const nothing: unknown = undefined
attempt("get", () => Reflect.get(nothing, "x"))
attempt("get", () => Reflect.get(Point, "name"))
