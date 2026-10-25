// Generic syntax the parser reports.
// expect-error: a function type cannot be generic itself: which copy of a generic function runs is decided when compiling, so a function value has fixed types; make the function that takes it generic instead, e.g. 'function apply<X>(f: (x: X) => X)'
function applyBoth(f: <X>(x: X) => X) { return 1 }
// expect-error: 'const' before a type parameter is not supported; remove it
function literal<const T>(x: T) { return x }
// expect-error: expected '>' to close the type parameters
function broken<T(x: T) { return x }
