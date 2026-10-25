// same-as-node
// Tuples: arrays of a fixed length with a type for each place. A tuple is a
// reference, as an array is in JavaScript, and prints as one.
function splitName(full: string): [string, string] {
  const i = full.indexOf(" ")
  return [full.slice(0, i), full.slice(i + 1)]
}
const [first, last] = splitName("Ann Lee")
console.log(first, last, splitName("Bob Stone"))

const t: [string, int] = ["Ann", 30]
console.log(t[0].length, t[1] + 1, t.length, t, typeof t)
t[1] = 31
t[0] += "!"
const same = t
same[1]++
console.log(t, same == t)

// Copied into an array with a spread, when the elements have one type.
const pair = splitName("A B")
const widened: [int, double] = [1, 2.5]
console.log([...pair, "C"], [...widened, 3])

// Tuples in arrays, objects and optionals, and of objects.
const grid: [int, int][] = []
for (let i = 0; i < 4; i++) grid.push([i, i * i])
console.log(grid, grid.map(([x, y]) => x + y))
for (const [x, y] of grid) if (x == 2) console.log(y)
const swap = (p: [string, int]): [int, string] => [p[1], p[0]]
console.log(swap(["x", 1]))
interface Item {
  name: string
}
const holder: { pair: [Item, int] } = { pair: [{ name: "first" }, 2] }
console.log(holder, holder.pair[0].name)
let maybe: [string, string] | undefined = undefined
console.log(maybe, maybe?.[0])
maybe = ["a", "b"]
if (maybe != undefined) console.log(maybe[1], maybe)
const fixed: readonly [int, int] = [1, 2]
console.log(fixed, fixed[0] + fixed[1])
const nested: [[int, string], boolean] = [[1, "one"], true]
console.log(nested, nested[0][1])
