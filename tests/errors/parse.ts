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
// expect-error: a rest parameter must be the last parameter
function r1(...xs: int[], y: int): void {}
// expect-error: a rest parameter cannot be optional or have a default value
function r2(...xs: int[] = []): void {}
// expect-error: 'readonly' applies only to array types
function ro(a: readonly int): void {}
// expect-error: a rest element must be the last one
const [...head, last] = [1, 2]
// expect-error: a rest element cannot have a default value
const [...all = []] = [1, 2]
{
  // expect-error: 'export' is allowed only at the top level
  export const inner = 1
}
// expect-error: '{' after '=>' starts the body of the function, so 'a:' is a label, and the function returns nothing
const object = () => { a: 1 }
