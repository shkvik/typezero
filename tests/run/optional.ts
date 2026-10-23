// same-as-node
// undefined, T | undefined, narrowing and ??.
function find(xs: int[], target: int): int | undefined {
  for (let i = 0; i < xs.length; i++) {
    if (xs[i] == target) return i
  }
  return undefined
}
function half(n: int): int | undefined {
  if (n % 2 == 0) return n / 2
}
const xs = [5, 8, 13, 21]
const at = find(xs, 13)
console.log(at, find(xs, 4))
if (at != undefined) console.log("found at", at + 1)
const missing = find(xs, 99)
console.log(missing == undefined, missing ?? -1, `${missing}`, `v=${missing}`)
console.log(half(10), half(7), half(7) ?? 0)
let maybe: string | undefined = undefined
console.log(maybe)
maybe ??= "set"
console.log(maybe, maybe.length)
let name: string | undefined = "Ann"
console.log(name == "Ann", name != undefined ? name.length : 0)
const words = ["x", "yy"]
let w = words[1]
console.log(w, words[5], words[5] ?? "none")
let total = 0
for (const v of [1, 2, 3]) total += v
console.log(total)
const grid = [[1, 2], [3, 4]]
console.log(grid[1][0], grid.length)
let count: int | undefined
console.log(count)
count = 3
console.log(count * 2)
