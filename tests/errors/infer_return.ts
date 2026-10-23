// Return types that cannot be inferred from the body.
function both(flag: boolean) {
  if (flag) return 1
  // expect-error: 'both' returns 'string' here and 'int' elsewhere; a function returns values of one type
  return "one"
}
// expect-error: cannot tell what 'forever' returns: every value it returns comes from a call to itself; declare its return type
function forever(n: int) {
  return forever(n + 1)
}
// expect-error: 'suffix' returns values of different types; a function returns values of one type
function suffix(n: int) {
  if (n == 0) return 0
  return suffix(n - 1) + "x"
}
// expect-error: 'nothing' returns only undefined; declare the return type, e.g. '): int | undefined'
function nothing(n: int) {
  if (n > 0) return undefined
}
function explicitVoid(): void {
  // expect-error: function 'explicitVoid' is declared to return 'void', but returns 'int'
  return 5
}

// Top-level code needs the return type before `limit` is declared.
console.log(usesLater())
function usesLater() {
  // expect-error: 'limit' is declared after the code that first calls 'usesLater', which needs to know what 'usesLater' returns; move the declaration up, or declare the return type
  return limit * 2
}
const limit = 10
