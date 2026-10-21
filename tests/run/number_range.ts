// expect-exit: 2
// expect-stderr: panic: the number 1500000000000 does not fit in int
// Where an int is expected, Math.round gives an int, and the program stops
// when the number does not fit: JavaScript would go on with one an int
// cannot hold.
let x = 1.5
for (let i = 0; i < 5; i++) {
  x *= 1000
  const n: int = Math.round(x)
  console.log(n)
}
