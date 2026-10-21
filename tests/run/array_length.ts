// expect-exit: 2
// expect-stderr: RangeError: Invalid array length
// A negative length JavaScript rejects with a RangeError ends the program.
function make(n: int): int[] {
  return new Array<int>(n)
}
console.log(make(2))
console.log(make(-1))
