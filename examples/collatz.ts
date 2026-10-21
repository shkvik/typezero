// Самая длинная последовательность Коллатца среди стартовых чисел меньше
// миллиона. Промежуточные значения не помещаются в int, поэтому они long.
function collatzLength(start: long): int {
  let n = start
  let steps = 1
  while (n != 1) {
    if (n % 2 == 0) n = n / 2
    else n = 3 * n + 1
    steps++
  }
  return steps
}

let best: long = 1
let bestLength = 1
for (let start: long = 1; start < 1000000; start++) {
  let length = collatzLength(start)
  if (length > bestLength) {
    best = start
    bestLength = length
  }
}
console.log("число:", best, "длина:", bestLength)
