// expect-error: function 'pick' must return a value of type 'int' on every path
function pick(flag: boolean): int {
  if (flag) return 1
}
