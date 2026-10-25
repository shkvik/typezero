let a = 1
// expect-error: duplicate declaration of 'a'
let a = 2

// Plain functions are not overloaded (docs/overloads.md).
function area(r: double): double {
  return r * r
}
// expect-error: duplicate declaration of 'area'; plain functions are not overloaded: give each variant its own name
function area(w: double, h: double): double {
  return w * h
}
{
  function inner(): int {
    return 1
  }
  // expect-error: duplicate declaration of 'inner'; plain functions are not overloaded
  function inner(x: int): int {
    return x
  }
}
