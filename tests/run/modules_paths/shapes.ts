import { Point } from "./geometry"
export { Point }
export interface Shape {
  kind: string
  at: Point
}
export function describe(s: Shape) {
  return `${s.kind} at ${s.at.x},${s.at.y}`
}
