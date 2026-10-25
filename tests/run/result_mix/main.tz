// Result together with the rest of the language: generic functions over
// Results (ok of a type parameter, type arguments inferred from Results,
// `ok` passed to a generic function, safeTry in a generic function, one copy
// for all object types), decimal values in both halves, objects and tuples
// in Results, and Results from another file.
import { Order, Parsed, parseName, parsePrice, parseQty } from "./orders"

function wrap<T>(x: T) {
  return ok(x)
}

function valueOr<T, E>(r: Result<T, E>, fallback: T): T {
  if (r.isOk()) return r.value
  return fallback
}

function mapAll<T, U>(xs: T[], f: (x: T) => U): U[] {
  const out: U[] = []
  for (const x of xs) out.push(f(x))
  return out
}

function pairUp<A, B>(a: Parsed<A>, b: Parsed<B>) {
  return safeTry(function* () {
    const first = yield* a
    const second = yield* b
    return ok({ first, second })
  })
}

function parseOrder(line: string): Parsed<Order> {
  const fields = line.split(";")
  if (fields.length != 3) return err(`expected 3 fields in '${line}'`)
  const name = fields[0], price = fields[1], qty = fields[2]
  // A tuple of the three values, which becomes an Order.
  return Result.combine([parseName(name), parsePrice(price), parseQty(qty)]).map(([n, p, q]) => ({
    name: n,
    price: p,
    qty: q,
  }))
}

// A decimal in either half: what is left of the budget, or how far over.
function checkBudget(total: decimal, budget: decimal): Result<decimal, decimal> {
  return total <= budget ? ok(budget - total) : err(total - budget)
}

console.log(wrap(1), wrap("two"), wrap(decimal(3)))
console.log(wrap({ id: 1 }), wrap({ label: "x", tags: ["a"] }))
console.log(valueOr(parseQty("7"), 1), valueOr(parseQty("-"), 1))
console.log(valueOr(parsePrice("x"), 0.1) + 0.2)
// Literals next to a decimal are exact decimals, also in callbacks.
const tip: Result<decimal, string> = ok(0.1)
console.log(tip.map(t => t + 0.2), tip.map(t => 0.2 + t).map(t => t == 0.3))
console.log(valueOr(ok("only"), "never used"), valueOr(err(404), "fallback"))
console.log(mapAll(["a", "b"], ok), mapAll([1], err))
console.log(pairUp(parseName("tea"), parseQty("2")))
console.log(pairUp(parseName(" "), parseQty("2")))
console.log(pairUp(parsePrice("1.5"), parsePrice("2.25")).map(p => p.first * p.second))
console.log(Result.combine([parseName("jam"), parseQty("3")]))

const lines = ["tea;4.20;3", "cake;12.5;2", ";1;1", "jam;1.2.3;1", "milk;0.99;x", "bread;2.10"]
const orders = mapAll(lines, parseOrder)
for (const r of orders) console.log(r)

let total = decimal(0)
for (const r of orders) total += r.map(o => o.price * o.qty).unwrapOr(0)
console.log(`total ${total}`)
console.log(checkBudget(total, 50), checkBudget(total, 30).mapErr(over => `over by ${over}`))

const all = Result.combineWithAllErrors(orders)
console.log(all.isErr(), all.mapErr(errors => errors.length).match(os => os.length, n => -n))
const good = orders.filter(r => r.isOk()).map(r => r.unwrapOr({ name: "", price: 0, qty: 0 }))
console.log(good.map(o => `${o.qty} x ${o.name}`).join(", "))
