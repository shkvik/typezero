// same-as-node
// Idioms of TypeScript code with dictionaries.
interface Item { id: string; n: int }
const items: Item[] = [{ id: "a", n: 2 }, { id: "b", n: 1 }]
const byId = Object.fromEntries(items.map((x): [string, Item] => [x.id, x]))
const asMap = new Map(items.map((x): [string, int] => [x.id, x.n]))
const counts: { [word: string]: number } = { x: 3, y: 1, z: 2 }
const sorted = Object.entries(counts).sort(([, a], [, b]) => b - a)
console.log(byId, asMap, sorted, Object.fromEntries(asMap))
const index = new Map<string, Item>()
for (const it of items) index.set(it.id, it)
const total = [...index.values()].reduce((s, it) => s + it.n, 0)
console.log(total, Math.max(...asMap.values()))
