// same-as-node --experimental-transform-types
// Classes go from file to file as the other declarations do.
import { Circle, Square as Box, largest } from "./shapes.ts"

const circles = [new Circle(1), new Circle(3), new Circle(2).grow(0.5)]
const big = largest(circles)
console.log(big, Circle.created, big?.area().toFixed(3))
const box: Box = new Box(2)
console.log(box.area(), box instanceof Box, box)
