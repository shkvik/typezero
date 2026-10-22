// same-as-node
// `?.` and `!`.
function count(a?: int[]): int | undefined {
  return a?.length
}
console.log(count([1, 2]), count())

const words = ["alpha", "beta"]
let at = 1
at = at * 3 - 2 // not proven in range
console.log(words[at]?.toUpperCase(), words[at + 5]?.toUpperCase(), words[at]?.length)

// A chain stops at the first undefined: nothing after it is evaluated.
let calls = 0
function next(): int {
  calls++
  return calls
}
function find(name: string): int[] | undefined {
  return name === "some" ? [7, 8, 9] : undefined
}
console.log(find("none")?.at(next()), calls, find("some")?.at(next()), calls)
console.log(find("none")?.length ?? 0, find("some")?.slice(1).length)

const grid: int[][] = [[1, 2], [3]]
let row = 1
row += 0
console.log(grid[row]?.[0], grid[row]?.[5], grid[row + 8]?.[0], grid[row]?.[0]?.toFixed(1))

// `x!` asserts that x is not undefined; here it is not.
const position = words.indexOf("beta")
const word = words[position]!
console.log(word.toUpperCase(), words.at(-1)!.length)
