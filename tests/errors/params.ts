// Optional parameters and default values.
// expect-error: a required parameter cannot follow an optional one
function f(a?: int, b: int): int {
  return b
}
function g(a: int, b: int = 2, c: string = "x"): void {}
// expect-error: expected 1-3 arguments, but got 0
g()
// expect-error: expected 1-3 arguments, but got 4
g(1, 2, "3", 4)
// expect-error: type 'string' is not assignable to type 'int'
function h(a: int = "zero"): void {}
// expect-error: parameter 'p' needs a type annotation
function m(p = [1]): void {}
// expect-error: cannot find name 'later'
function o(first: int = later, later: int = 1): void {}
function q(maybe?: string): int {
  // expect-error: the value may be undefined
  return maybe.length
}
