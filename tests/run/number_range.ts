// expect-exit: 1
// expect-stderr: RangeError: 1500000000000 does not fit in int
// Where an int is expected, Math.round gives an int, and throws a RangeError
// when the number does not fit: JavaScript would go on with one an int
// cannot hold.
let x = 1.5
for (let i = 0; i < 5; i++) {
  x *= 1000
  const n: int = Math.round(x)
  console.log(n)
}
