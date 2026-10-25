import { isOdd, shout, tagOf } from "./b.ts"
console.log("a loads")
export const prefix = "hello"
export function greet(who: string) {
  return shout(`${prefix}, ${who}`)
}
// Functions that call each other across the files, without return types.
export function isEven(n: int) {
  return n == 0 ? true : isOdd(n - 1)
}
console.log(tagOf("a"))
