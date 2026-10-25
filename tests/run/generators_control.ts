// same-as-node
// Generators: yield in branches and templates, labels, switch and do...while,
// values of every kind, many yields, generators emitted in one another.
function* branches(flag: boolean): Generator<int, void, int | undefined> {
  const x = flag ? yield 1 : yield 2
  console.log("x", x)
  const y = (yield 3) ?? 10
  console.log("y", y)
  const msg = `a${yield 4}b`
  console.log(msg)
}
for (const v of branches(true)) console.log("v", v)
const bo = branches(false)
console.log(bo.next().value, bo.next(7).value, bo.next(8).value, bo.next(9).value, bo.next().done)

function* labeled() {
  outer: for (let i = 0; i < 3; i++) {
    for (let j = 0; j < 3; j++) {
      if (j == 2) continue outer
      if (i == 2) break outer
      switch (j) {
        case 0:
          yield i * 10
          break
        default:
          yield i * 10 + j
      }
    }
  }
  let k = 0
  do {
    yield 100 + k
  } while (++k < 2)
}
console.log([...labeled()])
for (const v of labeled()) {
  switch (v) {
    case 10:
      continue
    case 101:
      break
    default:
      console.log("lab", v)
  }
}
const lo = labeled()
console.log([lo.next().value, lo.next().value, ...lo])

function* fns() {
  for (let i = 1; i <= 3; i++) yield (x: int) => x * i
}
console.log([...fns()].map(f => f(10)))
for (const f of fns()) console.log(f(2))
const callbacks: (() => int)[] = []
for (const f of fns()) callbacks.push(() => f(100))
console.log(callbacks.map(c => c()))

function* flags() {
  yield true
  yield false
  yield 1 > 0
}
console.log([...flags()], flags().filter(b => b).toArray().length)
function* longs(): Generator<long> {
  let x: long = 1
  for (let i = 0; i < 4; i++) {
    yield x
    x *= 1000000
  }
}
console.log([...longs()])
function* maybe(xs: int[]) {
  for (let i = 0; i < 5; i++) yield xs[i]
}
console.log([...maybe([1, 2])], maybe([3]).filter(x => x != undefined).map(x => x + 1).toArray())

// Many yields: an object even in a loop.
function* many() {
  yield 1
  yield 2
  yield 3
  yield 4
  yield 5
  yield 6
  yield 7
  yield 8
  yield 9
}
let total = 0
for (const m of many()) total += m
console.log(total)

// Deep chains of generators emitted in each other.
function* l1() {
  yield* [1, 2]
}
function* l2() {
  for (const x of l1()) yield x * 10
}
function* l3() {
  for (const x of l2()) yield x + 1
}
function* l4() {
  yield* l3()
}
function* l5() {
  for (const x of l4()) yield -x
}
console.log([...l5()])
for (const x of l5()) console.log("deep", x)

// take() inside yield* ends the delegation, not the generator.
function* naturals() {
  let n = 0
  while (true) yield n++
}
function* firstThree() {
  yield* naturals().take(3)
  yield* [7, 8, 9].values().drop(1).map(x => x * 2)
  yield 100
}
console.log([...firstThree()])
for (const x of firstThree()) console.log("x", x)
const ft = firstThree()
console.log(ft.next().value, ft.next().value, [...ft])
function* words() {
  const local = "a heap string, kept by the generator while it runs"
  for (const w of local.split(" ")) yield w
}
function* firstWords() {
  yield* words().take(2)
}
console.log([...firstWords()], firstWords().toArray())
