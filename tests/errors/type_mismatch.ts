// expect-error: type 'double' is not assignable to type 'int'
let x: int = 1.5
let big: long = 5
// expect-error: operator '+' cannot be applied to types 'long' and 'double'
let y = big + 0.5
// expect-error: integer literal does not fit in type 'int'
let z: int = 3000000000
