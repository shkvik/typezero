// same-as-node
// Reference counting of arrays and the allocator under load: no leaks, and
// memory goes back when objects die.
function make(n: int): string[] {
  const out: string[] = []
  for (let i = 0; i < n; i++) out.push(`s${i}`)
  return out
}
function total(rows: int[][]): int {
  let t = 0
  for (const row of rows) for (const v of row) t += v
  return t
}
function firstLong(words: string[]): string | undefined {
  for (const w of words) {
    if (w.length > 3) return w
  }
  return undefined
}
const global = make(3)
const alias = global
alias.push("added")
console.log(global, alias == global, make(0), firstLong(["ab", "abcd", "x"]), firstLong(global))
const grid: int[][] = []
for (let i = 0; i < 3; i++) {
  const row: int[] = []
  for (let j = 0; j < 3; j++) row.push(i * 3 + j)
  grid.push(row)
}
console.log(grid, total(grid))
if (grid.length >= 2) {
  grid[1] = [100]
  grid[0].push(7)
  const g0 = grid[0]
  console.log(grid, g0)
}
let swap = [["a"], ["b"]]
swap = [swap[1] ?? [], swap[0] ?? []]
console.log(swap)
for (const ch of "héllo😀!") {
  if (ch == "!") break
  console.log(ch, ch.length)
}
const strs = ["x", "y"]
strs[0] += "z"
strs[strs.length] = "w"
console.log(strs, strs.pop(), strs.shift(), strs)
const q: string[][] = [["a"], ["b"]]
while (q.length > 0) {
  const item = q.shift()
  console.log(item)
}
const keep = make(2)
keep.length = 0
console.log(keep, keep.length)
const opt: int[] | undefined = [1, 2]
console.log(opt, opt.length)
let maybeArr: string[] | undefined
console.log(maybeArr)
maybeArr ??= ["fresh"]
console.log(maybeArr, maybeArr[0])
const s = "hello"
for (let i = 0; i <= s.length; i++) console.log(s[i], s.charCodeAt(i), s.at(-i))

// Allocations of every size class, blocks above 32 KiB, and memory given back.
function churn(rounds: int): int {
  let check = 0
  for (let r = 0; r < rounds; r++) {
    const parts: string[] = []
    for (let i = 0; i < 2000; i++) parts.push(`${"x".repeat(i % 300 + 1)}${i}`)
    let size = 0
    for (const p of parts) size += p.length
    check = (check + size) % 1000007
    while (parts.length > 1000) parts.pop()
    check = (check + parts.join("").length) % 1000007
  }
  return check
}
console.log(churn(30))

let big = ""
for (let i = 0; i < 200000; i++) big += "ab"
console.log(big.length, big.slice(-3))

const huge: double[] = []
for (let i = 0; i < 3000000; i++) huge.push(i / 2)
console.log(huge.length, huge[huge.length - 1] ?? 0)
while (huge.length > 10) huge.pop()
huge.length = 2
console.log(huge)

const grids: int[][] = []
for (let g = 0; g < 50; g++) {
  const row: int[] = []
  for (let i = 0; i < 10000; i++) row.push(i ^ g)
  grids.push(row)
}
let x = 0
for (const row of grids) x ^= row[row.length - 1] ?? 0
grids.length = 0
console.log(x, grids.length)

const dq: string[] = []
for (let i = 0; i < 100000; i++) {
  if (i % 3 == 0) dq.unshift(`u${i}`)
  else dq.push(`p${i}`)
  if (i % 5 == 0) dq.shift()
}
console.log(dq.length, dq[0], dq[dq.length - 1])

// A const loop variable borrows its element only if nothing in the body can
// release elements; here pop() drops the element the variable holds.
const names = ["ann", "bob", "cy"]
const seen: string[] = []
for (const name of names) {
  names.pop()
  seen.push(name + "!")
}
let longest = ""
for (const name of seen) if (name.length > longest.length) longest = name
console.log(seen, names, longest)
