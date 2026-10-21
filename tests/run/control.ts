let sum: int = 0
for (let i = 0; i < 10; i++) {
  if (i % 2 == 0) continue
  if (i > 7) break
  sum += i
}
console.log("sum", sum)

let n: int = 0
while (true) {
  n++
  if (n >= 5) break
}
console.log("n", n)

let k: int = 10
do {
  k--
} while (k > 100)
console.log("k", k)

// break leaves only the innermost loop
let pairs: int = 0
for (let a = 0; a < 3; a++) {
  for (let b = 0; b < 3; b++) {
    if (b > a) break
    pairs++
  }
}
console.log("pairs", pairs)

for (let x = 0; x < 3; x++) {
  if (x == 0) console.log("zero")
  else if (x == 1) console.log("one")
  else console.log("many", x)
}

let s: int = 1
{
  let s: int = 2
  console.log("inner", s)
}
console.log("outer", s)

// && and || evaluate their right side only when needed
let calls: int = 0
function touch(v: boolean): boolean {
  calls++
  return v
}
console.log(touch(false) && touch(true), calls)
console.log(touch(true) || touch(true), calls)
console.log(calls > 1 ? 10 : 20, calls > 5 ? 1.5 : 2)
console.log(1 < 2 && 2 < 3, !(1 == 1), 3 != 4, 2.5 >= 2, true === true)
