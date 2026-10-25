// Results in generic functions: an instance for a Result type is checked by
// the rules for Results, and the error shows at the call that needs it.
function deep<T>(x: T, n: int): int {
  // expect-error: 'deep' calls itself with 'Result<T, never>' for 'T', so each copy of it for some type needs another for a bigger one
  return n == 0 ? 0 : deep(ok(x), n - 1)
}

function run<T>(f: () => T) {
  f()
}
// expect-error: cannot call 'run' with T = Result<int, never>: the Result is not used, so an error in it would go unchecked
run(() => ok(1))

function same<T>(a: T, b: T) {
  return a == b
}
// expect-error: cannot call 'same' with T = Result<int, never>: Results cannot be compared with '=='
console.log(same(ok(1), ok(1)))

function text<T>(x: T) {
  return `${x}`
}
// expect-error: cannot call 'text' with T = Result<int, string>: a Result cannot be converted to a string
console.log(text(err("x").map(n => n + 1)))
