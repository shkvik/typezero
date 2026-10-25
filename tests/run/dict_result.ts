// Results that hold dictionaries, and dictionaries of Results.
// A Result that holds one, and a dictionary of Results.
import { ok, err, Result } from "neverthrow"
function parse(text: string): Result<Map<string, int>, string> {
  const m = new Map<string, int>()
  for (const part of text.split(",")) {
    const [k, v] = part.split("=")
    if (k == undefined || v == undefined) return err(`bad part '${part}'`)
    m.set(k, Number(v) | 0)
  }
  return ok(m)
}
console.log(parse("a=1,b=2"), parse("a=1,b"))
const results = new Map<string, Result<int, string>>([["x", ok(1)], ["y", err("no")]])
console.log(results)
for (const [k, r] of results) console.log(k, r.match(v => `value ${v}`, e => `error ${e}`))
