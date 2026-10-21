// expect-error: type 'double' is not assignable to type 'int'
let x: int = 1.5
let big: long = 5
// A long becomes a double by itself, a double does not become a long.
// expect-error: type 'double' is not assignable to type 'long'; round it with Math.round
let y: long = big + 0.5
// expect-error: integer literal does not fit in type 'int'
let z: int = 3000000000
