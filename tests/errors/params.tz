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
// expect-error: cannot tell the type of 'p' from its default value
function m(p = undefined): void {}
// expect-error: the default value of 'first' uses 'later', which is not set yet
function o(first: int = later, later: int = 1): void {}
function q(maybe?: string): int {
  // expect-error: the value may be undefined
  return maybe.length
}
