// same-as-node
// Generators: objects left half-way, yield inside expressions, closures, nested
// generators, generics, lazy helpers kept as values.
function* words(prefix: string) {
  const local = `${prefix}-local`
  yield `${prefix}1`
  yield local
  yield `${prefix}3`
}

// Abandoned half-way: what the generator holds is released.
const w = words("w")
console.log(w.next().value, w.next().value)
const w2 = words("v")
w2.next()

// An object left with break.
const objectLoop = words("o")
for (const s of objectLoop) {
  if (s == "o-local") break
  console.log("got", s)
}

// yield inside expressions.
function* asked(): Generator<string, void, int> {
  const parts: string[] = []
  parts.push(`first ${yield "a"}`)
  console.log("parts", parts.length, `second ${yield "b"} and ${yield "c"}`)
}
const q = asked()
console.log(q.next().value, q.next(1).value, q.next(2).value, q.next(3).done)

// Default and rest parameters.
function* range(from: int, to = from + 3, step = 1) {
  for (let i = from; i < to; i += step) yield i
}
console.log([...range(2)], [...range(0, 10, 4)])
function* each(...xs: string[]) {
  for (const x of xs) yield x.length
}
console.log([...each("a", "bb", "ccc")])

// Closures inside a generator, over its variables.
function* counters() {
  let count = 0
  const bump = () => ++count
  while (count < 6) {
    bump()
    yield count
    bump()
  }
}
console.log([...counters()])
for (const c of counters()) console.log("c", c)

// Functions declared inside a generator.
function* fib() {
  function next(a: int, b: int) {
    return a + b
  }
  let a = 0, b = 1
  while (true) {
    yield a
    const c = next(a, b)
    a = b
    b = c
  }
}
console.log(fib().take(10).toArray())

// A generator emitted inside another one, which gives what it returns.
function* inner(n: int) {
  for (let i = 0; i < n; i++) yield i
  return n * 100
}
function* outer() {
  const a = yield* inner(2)
  const b = yield* inner(1)
  yield a + b
}
for (const x of outer()) console.log("outer", x)
console.log([...outer()])

// Generators in callbacks and through function values.
console.log([1, 2, 3].map(n => [...inner(n)]))
const make = inner
console.log([...make(3)])
const makers = [inner, inner]
console.log(makers.map(m => m(2).toArray()))

// A return in the middle; its value is dropped by for...of.
function* untilNegative(xs: int[]) {
  for (const x of xs) {
    if (x < 0) return x
    yield x
  }
  return 0
}
for (const x of untilNegative([1, 2, -1, 3])) console.log("pos", x)
const u = untilNegative([4, -2])
console.log(u.next().value, u.next().value, u.next().value)

// take(0) asks for nothing; materialized take and drop.
let pulled = 0
function* counted() {
  while (true) yield ++pulled
}
console.log(counted().take(0).toArray(), pulled)
const t2 = counted().take(2)
console.log([...t2], pulled, t2.next().done)
const d2 = counted().drop(2)
console.log(d2.next().value, pulled)
const fm = [1, 2].values().flatMap(n => [n, n])
console.log([...fm])

function* upTo(n: int) {
  for (let i = 0; i < n; i++) yield i
}
function* strs(n: int) {
  for (let i = 0; i < n; i++) yield `s${i}`
}

// Inside a function whose calls to itself are jumps.
function countDown(n: int, acc: int): int {
  if (n == 0) return acc
  let local = 0
  for (const i of upTo(n)) local += i
  return countDown(n - 1, acc + local)
}
console.log(countDown(5, 0))

// A generator object whose body iterates another generator in place.
function* nested() {
  for (const i of upTo(3)) {
    for (const s of strs(i)) yield `${i}:${s}`
  }
}
console.log([...nested()])
const nst = nested()
console.log(nst.next().value, nst.next().value)

// Destructuring assignment.
let a: int | undefined = 0, b: int | undefined = 0
;[a, b] = upTo(10)
console.log(a, b)
let first: string | undefined = "", second: string | undefined = ""
;[first, second] = strs(1)
console.log(first, second)

// Spread into a rest parameter.
function total(...xs: int[]) {
  return xs.reduce((s, x) => s + x, 0)
}
console.log(total(...upTo(5)), total(1, ...upTo(3), 10))

// switch inside the body.
for (const i of upTo(5)) {
  switch (i) {
    case 1:
      continue
    case 3:
      break
    default:
      console.log("i", i)
  }
  if (i == 3) break
}

// Iterators that may be undefined.
let maybe: IterableIterator<int> | undefined
console.log(maybe?.next().value)
maybe = upTo(2)
console.log(maybe?.next().value)
console.log([...maybe])

// Values that hold references: arrays and objects.
interface P { x: int; name: string }
function* points(): Generator<P> {
  for (let i = 0; i < 3; i++) yield { x: i, name: `p${i}` }
}
for (const p of points()) console.log(p.x, p.name)
console.log([...points()])
function* rows(): Generator<int[]> {
  yield [1, 2]
  yield []
}
console.log([...rows()], rows().map(r => r.length).toArray())

interface Point { x: int; y: int }
function* repeat<T>(x: T, times: int) {
  for (let i = 0; i < times; i++) yield x
}
const p: Point = { x: 1, y: 2 }
for (const q of repeat(p, 2)) console.log(q.x + q.y)
console.log([...repeat(p, 2)], repeat(p, 3).map(q => q.x * 10).toArray())
const r = repeat("s", 2)
console.log(r.next().value, [...r], [...r])
function* zip<A, B>(xs: A[], ys: B[]): Generator<[A, B]> {
  for (let i = 0; i < xs.length && i < ys.length; i++) yield [xs[i], ys[i]]
}
for (const [a, b] of zip([1, 2, 3], ["one", "two"])) console.log(a, b)
const firstPoints = repeat(p, 5).take(2).toArray()
console.log(firstPoints.length, firstPoints[0] == p)

function scaler(xs: int[], k: int, suffix: string) {
  return xs.values().map(x => `${x * k}${suffix}`).filter(s => s.length > 2)
}
const it = scaler([1, 20, 300], 10, "!")
console.log([...it])
function lazyWords(text: string) {
  const parts = text.split(" ")
  let seen = 0
  return parts.values().map(w => {
    seen++
    return `${seen}:${w}`
  })
}
const lw = lazyWords("a bb ccc")
console.log(lw.next().value)
console.log(lw.toArray())
