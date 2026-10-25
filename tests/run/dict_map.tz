// same-as-node
// Map: the methods of JavaScript, the order the keys were added in.
const ages = new Map<string, int>()
ages.set("ann", 31).set("bob", 25)
ages.set("cid", 40)
console.log(ages.size, ages.get("bob"), ages.get("zed"), ages.has("ann"), ages.has("x"))
console.log(ages)
ages.set("ann", 32) // keeps its place
console.log([...ages.keys()], [...ages.values()])
console.log(ages.delete("bob"), ages.delete("bob"), ages.size)
for (const [name, age] of ages) console.log(name, age)
for (const entry of ages.entries()) console.log(entry)
ages.forEach((age, name, map) => console.log(name, age, map.size))
console.log(Array.from(ages), [...ages])
ages.clear()
console.log(ages, ages.size)

// Built from pairs, an array of tuples, another map.
const colors = new Map([["red", 1], ["green", 2], ["blue", 3]])
const copy = new Map(colors)
copy.set("black", 0)
console.log(colors, copy)
const squares = new Map<int, int>([[1, 1], [2, 4]])
console.log(squares.get(2), squares.get(3) ?? -1)

// Keys of any type: numbers as SameValueZero, objects by reference.
const byNumber = new Map<double, string>()
byNumber.set(NaN, "nan").set(-0, "zero").set(1.5, "one and a half")
console.log(byNumber.get(NaN), byNumber.get(0), byNumber.has(-0), byNumber)
interface Point { x: int; y: int }
const p: Point = { x: 1, y: 2 }
const q: Point = { x: 1, y: 2 }
const names = new Map<Point, string>([[p, "p"]])
console.log(names.get(p), names.get(q), names.has(q))
const flags = new Map<boolean, int[]>()
flags.set(true, [1]).set(false, [])
flags.get(true)?.push(2)
console.log(flags)

// Modified while iterated: as JavaScript does.
const m = new Map([["a", 1], ["b", 2], ["c", 3]])
for (const [k, v] of m) {
  if (k == "a") m.delete("b")
  if (k == "c") m.set("d", 4)
  if (k == "d") m.set("a", 10) // an existing key stays where it is
  console.log(k, v)
}
console.log(m)
const n = new Map([["x", 1], ["y", 2]])
let steps = 0
for (const k of n.keys()) {
  steps++
  if (k == "x") {
    n.delete("x")
    n.set("x", 3) // deleted and added again: at the end
  }
  if (steps > 5) break
  console.log(k)
}
const cleared = new Map([[1, "a"], [2, "b"]])
for (const [k] of cleared) {
  console.log("at", k)
  if (k == 1) {
    cleared.clear()
    cleared.set(3, "c")
  }
}
console.log(cleared)

// Iterators as values.
const it = colors.keys()
console.log(it.next(), it.next())
console.log([...it])
const values = colors.values().map(v => v * 10).toArray()
console.log(values)
console.log(colors.entries().filter(([k, v]) => v > 1).map(([k]) => k).toArray())

// Map.groupBy.
const words = ["apple", "avocado", "banana", "blueberry", "cherry"]
const byLetter = Map.groupBy(words, w => w[0])
console.log(byLetter)
const byLength = Map.groupBy(words, (w, i) => w.length > 6)
console.log(byLength.get(true), byLength.get(false))

// Nested.
const nested = new Map<string, Map<string, int>>()
nested.set("a", new Map([["x", 1]]))
nested.get("a")?.set("y", 2)
console.log(nested, { m: nested }, [nested])
console.log(new Map<string, int>(), typeof m, m == m, m == n)
