// expect-exit: 1
// expect-stderr: RangeError: Invalid array length
// A negative length is a RangeError, as in JavaScript; nobody catches it here.
function make(n: int): int[] {
  return new Array<int>(n)
}
console.log(make(2))
console.log(make(-1))
