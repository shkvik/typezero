// Arrays work as in JavaScript, but elements are stored unboxed: an int[] is a
// plain buffer of 32-bit integers. Where the compiler can prove an index is in
// range, `a[i]` is a plain read with no check; elsewhere it is
// `T | undefined`, as in JavaScript, and the program decides what to do.

// Pascal's triangle: each row from the one above.
function pascal(rows: int): int[][] {
  const triangle: int[][] = [[1]]
  for (let r = 1; r < rows; r++) {
    const above = triangle[triangle.length - 1] ?? []
    const row = [1]
    for (let i = 1; i < above.length; i++) row.push(above[i - 1] + above[i])
    row.push(1)
    triangle.push(row)
  }
  return triangle
}
for (const row of pascal(6)) console.log(row.join(" "))

// How many words have each length. The check before counting is what lets
// the compiler accept the write: past the end, JavaScript would leave holes.
const words = ["the", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog"]
const byLength = new Array<int>(6).fill(0)
for (const word of words) {
  const n = word.length
  if (n < byLength.length) byLength[n]++
}
console.log("words by length:", byLength)
console.log("sorted:", words.slice().sort().join(", "))
console.log("first 'the' at", words.indexOf("the"), "last at", words.lastIndexOf("the"))

// A queue: shift() takes the first element in O(1).
const tasks = ["build", "test", "deploy"]
const done: string[] = []
while (tasks.length > 0) {
  const task = tasks.shift() // the loop condition proves it is there
  done.push(`${task}: ok`)
  if (task === "build") tasks.push("package")
}
console.log(done)
