// Classes of another file: exported and imported like the other
// declarations, with their static fields, methods and private members.
export class Circle {
  static created = 0
  constructor(public r: double) {
    Circle.created++
  }
  area() {
    return Math.PI * this.r * this.r
  }
  grow(by: double) {
    return new Circle(this.r + by)
  }
}

export class Square {
  #id = 0
  constructor(public side: double) {}
  area() {
    return this.side * this.side
  }
}

export function largest(circles: Circle[]) {
  let best = circles[0]
  for (const c of circles) if (best == undefined || c.area() > best.area()) best = c
  return best
}
