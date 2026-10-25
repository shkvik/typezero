// Shapes up a class chain, each told by a literal field `kind` of its own
// class, behind an interface; and a function that tests, used in other files.
export interface Measured {
  area(): double
  label(): string
}

export abstract class Shape implements Measured {
  constructor(public name: string, public made: Date) {}
  abstract area(): double
  label(): string {
    return `${this.name} (${this.area().toFixed(1)})`
  }
}

export class Circle extends Shape {
  kind: "circle" = "circle"
  constructor(name: string, made: Date, public r: double) {
    super(name, made)
  }
  area(): double {
    return 3 * this.r * this.r
  }
}

export class Rect extends Shape {
  kind: "rect" = "rect"
  constructor(name: string, made: Date, public w: double, public h: double) {
    super(name, made)
  }
  area(): double {
    return this.w * this.h
  }
}

export class Square extends Rect {
  constructor(name: string, made: Date, side: double) {
    super(name, made, side, side)
  }
  label(): string {
    return `square ${super.label()}`
  }
}

export type Kind = "circle" | "rect"
export type Figure = Circle | Rect

// Every kind handled: a new kind would be an error here.
export function corners(f: Figure): int {
  switch (f.kind) {
    case "circle":
      return 0
    case "rect":
      return 4
  }
}

export function isRound(f: Figure): f is Circle {
  return f.kind == "circle"
}
