// The other file of inherit_mix: an abstract class with a generator method
// and a regex in a static field, subclasses that override them, an
// interface of fields only, and a function that returns a Result.
export interface Named {
  name: string
}

export abstract class Shape implements Named {
  static size = /^(\d+)x(\d+)$/
  id = 0
  constructor(public name: string) {}
  abstract area(): double
  *corners(): Generator<string> {
    yield `${this.name}: no corners`
  }
  describe() {
    return `${this.name} ${this.area().toFixed(1)}`
  }
}

export class Rect extends Shape {
  label = /[a-z]+/
  constructor(public w: double, public h: double) {
    super("rect")
  }
  area() {
    return this.w * this.h
  }
  *corners(): Generator<string> {
    for (const c of ["tl", "tr", "br", "bl"]) yield `${this.name}: ${c}`
  }
  scale(k: int) {
    return new Rect(this.w * k, this.h * k)
  }
  scale(k: decimal) {
    return new Rect(this.w * Number(k), this.h * Number(k))
  }
  // "3x4" makes a rectangle.
  static parse(text: string): Result<Rect, string> {
    const m = Shape.size.exec(text)
    if (m == undefined) return err(`not a size: '${text}'`)
    // A regex in a field is known only when the program runs: its groups may be
    // undefined.
    return ok(new Rect(parseInt(m[1] ?? "0"), parseInt(m[2] ?? "0")))
  }
}

export class Circle extends Shape {
  constructor(public r: double) {
    super("circle")
  }
  area() {
    return 3 * this.r * this.r
  }
}

export function largest(shapes: Shape[]): Result<Shape, string> {
  const first = shapes[0]
  if (first == undefined) return err("no shapes")
  let best = first
  for (const s of shapes) if (s.area() > best.area()) best = s
  return ok(best)
}
