// same-as-node
// Methods of arrays that take callbacks.
const nums = [5, 3, 8, 1, 9, 2]
console.log(nums.map((x, i) => x * i))
console.log(nums.map((x, i, all) => all.length - i + x))
console.log(nums.filter((x, i) => i % 2 == 0 && x > 2))
console.log(nums.reduce((a, b) => a + b))
console.log(nums.reduce((acc, x, i) => acc + x * i, 0))
console.log(nums.reduceRight((acc, x) => `${acc},${x}`, ""))
console.log([1.5, 2.5].reduce((a, b) => a + b, 0))
console.log(nums.find(x => x > 5), nums.find(x => x > 50))
console.log(nums.findLast(x => x < 5), nums.findLastIndex(x => x > 100), nums.findIndex(x => x == 8))
const none: int[] = []
console.log(nums.some(x => x > 8), nums.every(x => x > 0), none.every(x => x > 0))
console.log(nums.flatMap(x => [x, x * 10]).length, nums.flatMap(x => x + 1))
let visited = 0
nums.forEach((x, i, all) => {
  visited += x
  if (i == 0) all.push(100) // not visited: added after the start
})
console.log(visited, nums.length)
nums.pop()

// sort with a comparison function: stable, numbers, strings, doubles.
const people = [["ann", "30"], ["bob", "25"], ["cid", "30"], ["dan", "25"]]
people.sort((a, b) => parseInt(a[1] ?? "0") - parseInt(b[1] ?? "0"))
console.log(people.map(p => p[0]).join(" "))
const values = [3.5, -1, 2.25, 10]
values.sort((a, b) => b - a)
console.log(values)
const words = ["pear", "Fig", "apple", "banana"]
console.log(words.sort((a, b) => a.length - b.length))
const maybe: (int | undefined)[] = [3, undefined, 1, 2]
maybe.sort((a, b) => (a ?? 0) - (b ?? 0))
console.log(maybe)

// Built-in functions where a callback is expected.
console.log(["1.5", "2", "x"].map(parseFloat), [4, 9, 16].map(Math.sqrt))
console.log([-1.5, 2.5].map(Math.round), ["a", "b"].map(String))
console.log([1, NaN, 3].filter(isNaN).length)
const strs = ["10", "20"]
console.log(strs.map(s => parseInt(s)))

// Named functions as callbacks.
function isEven(x: int) { return x % 2 == 0 }
function describe(x: int, i: int) { return `${i}:${x}` }
console.log(nums.filter(isEven), nums.map(describe))
function total(acc: int, x: int) { return acc + x }
console.log(nums.reduce(total, 0))

// Chains.
const result = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
  .filter(x => x % 3 != 0)
  .map(x => x * x)
  .reduce((a, b) => a + b, 0)
console.log(result)

// Callbacks capturing variables, changing them.
let count = 0
const big = nums.filter(x => {
  count++
  return x > 4
})
console.log(big, count)
const matrix = [[1, 2], [3, 4]]
console.log(matrix.map(row => row.map(x => x * 10)))
console.log(matrix.map(row => row.reduce((a, b) => a + b, 0)))
