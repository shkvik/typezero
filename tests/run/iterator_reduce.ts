// expect-exit: 1
// expect-stderr: TypeError: Reduce of a done iterator with no initial value
// reduce() without an initial value over an iterator that gives nothing.
function* none(): Generator<int> {
  return
}
console.log([1, 2].values().reduce((a, b) => a + b))
console.log(none().reduce((a, b) => a + b, 7))
console.log(none().reduce((a, b) => a + b))
