// same-as-node
// Generators: function*, yield, yield*, next(x), as plain loops and as objects.
function* naturals() {
  let n = 0
  while (true) yield n++
}

function* evens(limit: int) {
  for (let i = 0; i < limit; i += 2) yield i
}

function* words(): Generator<string> {
  yield "alpha"
  yield `be${"ta"}`
  yield "gamma".toUpperCase()
}

// A plain loop: the generator is emitted in place.
let sum = 0
for (const e of evens(1000)) sum += e
console.log("sum", sum)

// An endless generator, left with break.
for (const n of naturals()) {
  if (n > 3) break
  console.log("n", n)
}

// Strings, which are counted.
const all: string[] = []
for (const w of words()) all.push(w)
console.log(all.join(","))

// continue and labels.
outer: for (const a of evens(6)) {
  for (const b of evens(6)) {
    if (b > a) continue outer
    if (a == 4 && b == 2) break outer
    console.log("pair", a, b)
  }
}

// return from inside the loop.
function firstOver(limit: int) {
  for (const n of naturals()) if (n * n > limit) return n
  return -1
}
console.log("firstOver", firstOver(50))

// As an object.
const g = evens(5)
console.log(g, typeof g)
const r1 = g.next()
console.log(r1.value, r1.done)
console.log(g.next().value, g.next().value)
const r2 = g.next()
console.log(r2.value, r2.done)

// Spread, Array.from and destructuring.
console.log([...evens(9)])
console.log(["x", ...words(), "end"].length)
console.log(Array.from(evens(7)))
console.log(Array.from(words(), (w, i) => `${i}:${w}`))
const [a, b] = naturals()
console.log(a, b)
const [w1, ...rest] = words()
console.log(w1, rest)

// A generator that returns a value.
function* countdown(from: int) {
  for (let i = from; i > 0; i--) yield i
  return 100
}
const c = countdown(2)
console.log(c.next().value, c.next().value, c.next().value, c.next().done)
for (const x of countdown(3)) console.log("count", x)

// What next(x) sends is what `yield` gives.
function* talk(): Generator<string, void, string> {
  const name = yield "who are you?"
  console.log("hello,", name)
  const city = yield `where do you live, ${name}?`
  console.log(name, "lives in", city)
}
const t = talk()
console.log(t.next().value)
console.log(t.next("Anna").value)
console.log(t.next("Paris").done)

// A running total of what is sent.
function* totals(): Generator<int, void, int | undefined> {
  let total = 0
  while (true) {
    const x = yield total
    total += x ?? 0
  }
}
const s = totals()
s.next()
s.next(5)
s.next()
console.log("total", s.next(10).value)

// yield* of arrays, strings and generators, and what they return.
function* inner() {
  yield 1
  yield 2
  return 10
}
function* outer() {
  yield 0
  const got = yield* inner()
  yield got
  yield* [20, 30]
  yield* "ab".length == 2 ? [40] : [50]
}
console.log([...outer()])
function* letters() {
  yield* "héllo😀"
}
console.log([...letters()])

// A tree, walked recursively.
interface Tree {
  value: int
  left?: Tree
  right?: Tree
}
function* walk(t: Tree): Generator<int> {
  if (t.left) yield* walk(t.left)
  yield t.value
  if (t.right) yield* walk(t.right)
}
const tree: Tree = { value: 4, left: { value: 2, left: { value: 1 }, right: { value: 3 } }, right: { value: 6 } }
console.log([...walk(tree)])
for (const v of walk(tree)) if (v % 2 == 0) console.log("even", v)

// Generators nested in a function, which capture its variables.
function counter(start: int, step: int) {
  function* run() {
    let n = start
    while (true) {
      yield n
      n += step
    }
  }
  return run()
}
const ctr = counter(10, 5)
console.log(ctr.next().value, ctr.next().value, ctr.next().value)
const squares = function* (n: int) {
  for (let i = 1; i <= n; i++) yield i * i
}
console.log([...squares(4)])

// Generic generators.
function* repeat<T>(x: T, times: int) {
  for (let i = 0; i < times; i++) yield x
}
console.log([...repeat("ab", 3)], [...repeat(1.5, 2)])
function* pairs<T>(xs: T[]): Generator<[int, T]> {
  for (let i = 0; i < xs.length; i++) yield [i, xs[i]]
}
for (const [i, w] of pairs(["x", "y"])) console.log(i, w)

// Iterators passed around.
function sumAll(it: IterableIterator<int>) {
  let total = 0
  for (const x of it) total += x
  return total
}
console.log(sumAll(inner()), sumAll([1, 2, 3].values()))
const its = [inner(), inner()]
console.log(its[0] == its[1], its[0] == its[0], its)

// The same generator in nested loops.
function* upTo(n: int) {
  for (let i = 0; i < n; i++) yield i
}
let grid = ""
for (const i of upTo(3)) {
  for (const j of upTo(i + 1)) grid += `${i}${j} `
}
console.log(grid)
