// same-as-node
// Unions of other values hold a tag: `string | int` is the string or the
// int, unboxed, and typeof tells which.
function describe(x: string | int | boolean) {
  if (typeof x == "string") return `text of ${x.length}`
  if (typeof x == "boolean") return x ? "yes" : "no"
  return `number ${x * 2}`
}
console.log(describe("abc"), describe(21), describe(true), describe(false))

let v: string | int = 5
console.log(v, typeof v, `${v}`, String(v))
v = "five"
console.log(v, typeof v, v.length)

const mixed: (string | int)[] = [1, "a", 2, "b", 3]
console.log(mixed, mixed.length, mixed.includes("a"), mixed.includes(4), mixed.indexOf(2))
console.log(mixed.join("-"), JSON.stringify(mixed), mixed.filter(x => typeof x == "string"))
const counted = mixed.map(x => (typeof x == "string" ? x.length : x))
console.log(counted)

// Undefined too: `string | int | undefined` is the same value, with a tag
// for undefined.
let maybe: string | int | undefined
console.log(maybe, maybe ?? "none", maybe == undefined)
maybe = 7
console.log(maybe, maybe ?? "none", typeof maybe)
function firstText(xs: (string | int)[]) {
  for (const x of xs) if (typeof x == "string") return x
  return undefined
}
console.log(firstText(mixed), firstText([1, 2]))

// Unions found from the values: arrays, branches, returns.
const pair = ["Ann", 30]
const choice = pair.length > 1 ? 1 : "one"
function parse(text: string) {
  if (text == "") return undefined
  const n = Number(text)
  return Number.isNaN(n) ? text : n
}
console.log(pair, choice, parse(""), parse("12"), parse("twelve"))
console.log(Object.values({ name: "Ann", age: 30, admin: false }))
console.log(Object.entries({ id: 1, tag: "x" }))

// Generators that yield values of several types, and next() of one that
// returns another type.
function* tokens() {
  yield 1
  yield "plus"
  yield 2
}
console.log([...tokens()])
function* countdown(n: int) {
  for (let i = n; i > 0; i--) yield i
  return "liftoff"
}
const it = countdown(2)
console.log(it.next(), it.next(), it.next())

// Functions and arrays as members.
function run(task: (() => string) | string[]) {
  return typeof task == "function" ? task() : task.join(" ")
}
console.log(run(() => "called"), run(["a", "list"]))
function lengthOf(x: string | string[]) {
  return Array.isArray(x) ? `${x.length} items` : `${x.length} chars`
}
console.log(lengthOf("abc"), lengthOf(["a", "b"]))

// A union in a field, printed and written as JSON.
type Cell = { id: int; value: string | int | boolean }
const cells: Cell[] = [{ id: 1, value: "x" }, { id: 2, value: 3 }, { id: 3, value: true }]
console.log(cells, JSON.stringify(cells))

// In static fields, tuples, destructuring, `??=`, and `+=` of a narrowed
// variable.
class Config {
  static mode: "dev" | "prod" = "dev"
  static last: string | int | undefined = undefined
}
Config.mode = "prod"
Config.last = 3
console.log(Config.mode, Config.last)
interface Circle { kind: "circle"; r: double }
function radius(x: "none" | Circle) { return x == "none" ? 0 : x.r }
console.log(radius("none"), radius({ kind: "circle", r: 2 }))
const box: { v?: string | int } = {}
box.v ??= "init"
console.log(box, box.v)
const [p, q] = mixed
console.log(p, q, [...mixed, 4], Array.from(mixed).length)
const switchState: [string | int, "on" | "off"] = ["x", "on"]
console.log(switchState, switchState[1] == "on")
let total: string | int = 0
const add = (n: int) => {
  if (typeof total == "number") total += n
}
add(2)
add(3)
console.log(total)
