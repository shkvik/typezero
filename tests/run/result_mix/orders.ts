// The other file of result_mix: functions that return Results, a generic
// alias of Result and an interface, all imported by main.ts. neverthrow's
// err comes in under another name.
import { err as fail } from "neverthrow"

export interface Order {
  name: string
  price: decimal
  qty: int
}

export type Parsed<T> = Result<T, string>

export function parseName(s: string): Parsed<string> {
  const name = s.trim()
  return name.length == 0 ? fail("empty name") : ok(name)
}

// Digits with at most one point: decimal("1.2.3") would be a SyntaxError.
export function parsePrice(s: string): Parsed<decimal> {
  let points = 0
  for (const c of s) {
    if (c == ".") points++
    else if (c < "0" || c > "9") return fail(`bad price '${s}'`)
  }
  if (s.length == 0 || points > 1) return fail(`bad price '${s}'`)
  return ok(decimal(s))
}

export function parseQty(s: string): Parsed<int> {
  const n = parseInt(s)
  return isNaN(n) || n <= 0 ? fail(`bad quantity '${s}'`) : ok(n | 0)
}
