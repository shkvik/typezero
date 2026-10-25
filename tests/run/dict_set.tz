// same-as-node
// Set: the methods of JavaScript and of ES2025.
const s = new Set<int>()
s.add(3).add(1).add(3)
console.log(s, s.size, s.has(1), s.has(2))
console.log(s.delete(3), s.delete(3), s)
const letters = new Set("hello")
console.log(letters, [...letters].join(""))
const fromArray = new Set([5, 1, 5, 2, 1])
console.log(fromArray, Array.from(fromArray), [...fromArray.values()], [...fromArray.keys()])
console.log([...fromArray.entries()])
fromArray.forEach((v, again, set) => console.log(v, again, set.size))
for (const x of fromArray) console.log(x)
const [first, second] = fromArray
console.log(first, second)

const a = new Set([1, 2, 3, 4])
const b = new Set([3, 4, 5])
console.log(a.union(b), a.intersection(b), a.difference(b), a.symmetricDifference(b))
console.log(b.intersection(a), b.difference(a))
console.log(a.isSubsetOf(b), new Set([3]).isSubsetOf(b), a.isSupersetOf(new Set([1, 2])))
console.log(a.isDisjointFrom(b), a.isDisjointFrom(new Set([9])))
const keys = new Map([[1, "one"], [9, "nine"]])
console.log(a.intersection(keys), a.union(keys))

// Strings, objects and undefined as elements.
const words = new Set(["b", "a", "b", "c"])
console.log(words, words.has("a"))
const maybe = new Set<string | undefined>(["x", undefined])
console.log(maybe.has(undefined), maybe.has("y"), maybe)
interface Item { id: int }
const one: Item = { id: 1 }
const items = new Set([one, one, { id: 1 }])
console.log(items.size, items)

// Deduplicating, the usual use.
const tags = ["ts", "go", "ts", "rust", "go"]
console.log([...new Set(tags)])
const seen = new Set<string>()
for (const t of tags)
  if (seen.has(t)) console.log("again", t)
  else seen.add(t)
s.clear()
console.log(s, s.size, new Set<string>())
const big = new Set<int>()
for (let i = 0; i < 1000; i++) big.add(i % 300)
let sum = 0
for (const x of big) sum += x
console.log(big.size, sum)
for (let i = 0; i < 290; i++) big.delete(i)
console.log(big)
const it = big.values()
it.next()
console.log(it, it.next())
console.log(new Set([1, 2]).entries())
