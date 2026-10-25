// expect-exit: 1
// expect-stderr: RangeError: -2 must be positive
// take() and drop() with a negative count throw a RangeError, as in
// JavaScript; a count of 0 is fine.
function* naturals() {
  let n = 0
  while (true) yield n++
}
console.log(naturals().take(0).toArray(), naturals().drop(0).take(1).toArray())
let count = -2
console.log(naturals().take(count).toArray())
console.log("not reached")
