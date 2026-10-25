import { pong } from "./pong"
export function countdown(n: int): string {
  return n == 0 ? "done" : pong(n - 1)
}
