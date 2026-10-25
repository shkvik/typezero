// same-as-node
// Labels let break and continue leave several loops at once.
const grid = [
  [1, 2, 3],
  [4, -5, 6],
  [7, 8, 9],
]
let found = ""
search: for (let r = 0; r < grid.length; r++) {
  const row = grid[r]
  for (let c = 0; c < row.length; c++) {
    if (row[c] < 0) {
      found = `${r},${c}`
      break search
    }
  }
}
console.log("negative at", found)

// continue with a label goes on with the outer loop.
const pairs: string[] = []
outer: for (let i = 0; i < 4; i++) {
  for (let j = 0; j < 4; j++) {
    if (j > i) continue outer
    if ((i + j) % 3 == 0) continue
    pairs.push(`${i}${j}`)
  }
}
console.log(pairs.join(" "))

// while and do...while, and a label on a for...of.
let n = 0
loop: while (true) {
  do {
    n++
    if (n > 5) break loop
  } while (n % 2 != 0)
  console.log("even", n)
}
words: for (const word of ["ab", "cd", "ef"]) {
  for (const ch of word) {
    if (ch == "d") continue words
    console.log(ch)
  }
}

// A labeled block: break leaves it.
function classify(x: int): string {
  let result = "big"
  check: {
    if (x < 0) {
      result = "negative"
      break check
    }
    if (x < 10) {
      result = "small"
      break check
    }
    console.log("checked", x)
  }
  return result
}
console.log(classify(-1), classify(3), classify(50))

// break with a label from a switch inside a loop.
let steps = 0
scan: for (let i = 0; i < 10; i++) {
  switch (i) {
    case 3:
      break scan
    default:
      steps++
  }
}
console.log("steps", steps)

// An endless loop that only a labeled break leaves.
function firstSquareAbove(limit: int): int {
  let k = 0
  done: while (true) {
    k++
    if (k * k > limit) break done
  }
  return k
}
console.log(firstSquareAbove(50))
