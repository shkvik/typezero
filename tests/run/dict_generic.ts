// same-as-node
// Dictionaries in classes, generic functions and Results.
class Inventory {
  private items = new Map<string, int>()
  readonly tags: Set<string> = new Set()
  add(name: string, n: int) {
    this.items.set(name, (this.items.get(name) ?? 0) + n)
    this.tags.add(name[0] ?? "")
    return this
  }
  count(name: string) { return this.items.get(name) ?? 0 }
  total() {
    let sum = 0
    for (const n of this.items.values()) sum += n
    return sum
  }
  names() { return [...this.items.keys()] }
}
const inv = new Inventory().add("apple", 3).add("pear", 2).add("apple", 4)
console.log(inv.count("apple"), inv.count("kiwi"), inv.total(), inv.names(), inv.tags)
console.log(inv)

function countAll<T>(xs: T[]): Map<T, int> {
  const counts = new Map<T, int>()
  for (const x of xs) counts.set(x, (counts.get(x) ?? 0) + 1)
  return counts
}
console.log(countAll([1, 2, 1, 3, 1]), countAll(["a", "b", "a"]))
interface User { name: string; team: string }
const users: User[] = [{ name: "ann", team: "red" }, { name: "bob", team: "blue" }, { name: "cid", team: "red" }]
console.log(countAll(users.map(u => u.team)))
function invert<K, V>(m: Map<K, V>): Map<V, K> {
  const out = new Map<V, K>()
  for (const [k, v] of m) out.set(v, k)
  return out
}
console.log(invert(new Map([["one", 1], ["two", 2]])))
function lookup<V>(r: Record<string, V>, key: string, fallback: V): V {
  return r[key] ?? fallback
}
const prices: Record<string, double> = { tea: 2.5, cake: 4 }
console.log(lookup(prices, "tea", 0), lookup(prices, "jam", -1))
function firstKey<K, V>(m: Map<K, V>): K | undefined {
  for (const k of m.keys()) return k
  return undefined
}
console.log(firstKey(new Map([[true, "yes"]])), firstKey(new Map<int, int>()))
