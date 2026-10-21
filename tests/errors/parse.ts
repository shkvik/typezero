// Mistakes the parser reports and goes on.
const x: int = 2
// expect-error: the left side of '**' cannot be a unary '-' expression; add parentheses, e.g. '(-x) ** 2'
const a = -x ** 2
// expect-error: the left side of '**' cannot be a unary 'typeof' expression; add parentheses, e.g. '(typeof x) ** 2'
const b = typeof x ** 2
// expect-error: a parameter with a default value is optional already; remove the '?'
function k(a?: int = 1): void {}
// expect-error: generic type aliases are not supported yet
type Box<T> = T[]
switch (x) {
  default:
    break
  // expect-error: a 'switch' can have only one 'default'
  default:
    break
}
