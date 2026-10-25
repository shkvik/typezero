import { add } from "./math.ts"
import { unit } from "./geometry/index.ts"
console.log("describe loads")
export function describe(n: int) {
  return `${add(n, n)} ${unit}`
}
