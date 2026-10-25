// same-as-node
// filter(Boolean) and filter(x => x != undefined) keep the elements that
// hold a value, and the type of the result says so: `(T | undefined)[]`
// becomes `T[]` (docs/small.md, section 1).
interface Item { id: int }
const words = ["a", undefined, "", "b"]
const kept: string[] = words.filter(Boolean)
console.log(kept, kept.map(w => w.length))
const counts: (int | undefined)[] = [1, undefined, 0, 3]
const defined = counts.filter(x => x != undefined)
console.log(defined, defined.map(n => n * 2))
const items: (Item | undefined)[] = [{ id: 1 }, undefined, { id: 2 }]
console.log(items.filter(Boolean).map(i => i.id), items.filter(i => i != undefined && i.id > 1))
console.log(items.filter(i => !!i).length, items.filter(i => typeof i != "undefined").length)
const blocks = [() => 1, undefined]
console.log(blocks.filter(f => f != undefined).map(f => f()))
function compact<T>(xs: (T | undefined)[]) { return xs.filter(x => x != undefined) }
console.log(compact([1, undefined, 3]), compact(["x", undefined]), compact(items))
