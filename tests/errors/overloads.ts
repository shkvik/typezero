// Plain functions are not overloaded: TypeScript's overload signatures over
// one body are an error, as is a declaration without a body.
// expect-error: overload signatures are not supported: plain functions are not overloaded; give each variant its own name and body
function pick(a: int): int
function pick(a: int, b: int): int
function pick(a: int, b?: int): int {
  return b ?? a
}
// expect-error: overload signatures are not supported
export function exported(a: int): int
export function exported(a: int): int {
  return a
}
{
  // expect-error: overload signatures are not supported
  function nested(): int;
  function nested(): int {
    return 1
  }
}
// expect-error: function 'lonely' has no body; add it after the parameters: '{ ... }'
function lonely(a: int): int
const afterLonely = 1
// expect-error: function 'first' has no body; add it after the parameters: '{ ... }'
function first(a: int): int
function second(a: int): int {
  return a
}
