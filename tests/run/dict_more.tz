// same-as-node
// Dictionaries wherever values are iterated, passed and kept.
const m = new Map([["a", 1], ["b", 2], ["c", 3]])
const [entry, second] = m
console.log(entry, second)
function* both() {
  yield* m.keys()
  yield* new Set(["x", "y"])
}
console.log([...both()], Array.from(m.values()), Iterator.from(m).toArray())
console.log(Array.from(new Set([3, 4]), x => x * 2))
console.log(m.keys().some(k => k == "b"), m.values().reduce((a, b) => a + b, 0))
console.log([...m.entries()].sort((x, y) => y[1] - x[1]).map(([k]) => k))

interface Dict { [key: string]: int }
const d: Dict = { one: 1 }
d.two = 2
console.log(d, new Map(Object.entries(d)))
for (const [k, v] of Object.entries(d)) console.log(k, v)
class Registry {
  byName: Record<string, int[]> = {}
  index = new Map<int, string>()
  add(name: string, id: int) {
    (this.byName[name] ??= []).push(id)
    this.index.set(id, name)
  }
}
const reg = new Registry()
reg.add("a", 1)
reg.add("a", 2)
reg.add("b", 3)
console.log(reg, reg.index.get(2), reg.byName.a?.length)

let maybe: Map<string, int> | undefined
console.log(maybe?.get("a"), maybe == undefined)
maybe = m
console.log(maybe?.get("a"), maybe?.size, maybe == m)
const lists = new Map<string, string[]>()
lists.set("x", [])
lists.get("x")?.push("one")
lists.get("y")?.push("none")
console.log(lists)

function keysOf<V>(r: Record<string, V>) {
  return Object.keys(r)
}
function first<T>(xs: T[]): T | undefined {
  return xs[0]
}
console.log(keysOf({ x: 1, y: 2 }), keysOf<string>({}), first([m, new Map<string, int>()]))
const sets = [new Set([1]), new Set([2, 3])]
console.log(sets.map(s => s.size), sets.indexOf(sets[1] ?? new Set<int>()), sets)
const tuples = new Map<[int, int], string>()
const origin: [int, int] = [0, 0]
tuples.set(origin, "origin").set([0, 0], "another")
console.log(tuples.size, tuples.get(origin), tuples)
const records: Record<string, int>[] = [{ a: 1 }, {}]
console.log(records, records.map(r => Object.keys(r).length))

// An object literal takes its type from the dictionary it goes into.
interface Stat { count: number; total: number }
const stats = new Map<string, Stat>()
const firstStat = { count: 0, total: 0 }
stats.set("a", firstStat)
firstStat.count += 1.5
const byName: Record<string, Stat> = {}
const named = { count: 1, total: 2 }
byName["x"] = named
byName.y = named
const points = new Set<{ x: number }>()
const pt = { x: 1 }
points.add(pt)
console.log(stats, byName, points, points.has(pt))
