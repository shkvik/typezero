// A class that extends a class of another file, and an interface of another
// file implemented in this one (node imports types only with `import type`,
// so this test is not run by node).
import { Shape, Circle, HasArea } from "./shapes.ts"

class Square extends Shape {
  constructor(public side: double) {
    super("square")
  }
  area() {
    return this.side * this.side
  }
}

class Room implements HasArea {
  constructor(public w: double, public h: double) {}
  area() {
    return this.w * this.h
  }
}

function largest<T>(items: T[], size: (x: T) => double) {
  let best = items[0]
  for (const item of items) if (best == undefined || size(item) > size(best)) best = item
  return best
}

const shapes: Shape[] = [new Circle(1), new Square(3), new Circle(2)]
const big = largest(shapes, s => s.area())
if (big) console.log(big.describe())
const areas: HasArea[] = [...shapes, new Room(2, 5)]
console.log(areas.map(a => a.area().toFixed(2)).join(" "))
console.log(shapes.filter(s => s instanceof Circle).length, areas[3] instanceof Room)
