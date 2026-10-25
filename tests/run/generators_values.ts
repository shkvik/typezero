// same-as-node
// Iterators as values: in fields, arrays, closures and generic functions.
function* upTo(n: int) {
  for (let i = 0; i < n; i++) yield i
}
interface Holder {
  name: string
  it: IterableIterator<int>
}
const h: Holder = { name: "h", it: upTo(3) }
console.log(h)
console.log(h.it.next().value, [...h.it])
function first<T>(xs: T[]): T | undefined {
  return xs[0]
}
const gens = [upTo(2), upTo(3)]
const f = first(gens)
console.log(f, f?.next().value)
let saved = upTo(4)
const step = () => saved.next().value
console.log(step(), step())
saved = upTo(1)
console.log(step(), step())
const many: IterableIterator<string>[] = []
for (let i = 0; i < 3; i++) many.push(["a", "b", "c"].values().drop(i))
console.log(many.map(m => [...m].join("")))

// A generator may change what the code around next() holds: that is read
// before the generator runs, and kept.
let label = "a label long enough to live on the heap"
function* changer() {
  label = "another label long enough to live on the heap"
  yield 1
}
const ch = changer()
console.log(label, ch.next().value, label)
const parts = [label.length]
for (const x of changer()) parts.push(x, label.length)
console.log(parts, [label.length, ...changer()].length)
