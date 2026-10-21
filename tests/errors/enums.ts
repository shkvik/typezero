// Enums and type aliases.
// expect-error: enum 'Mixed' cannot mix numbers and strings
enum Mixed {
  A = 1,
  B = "b",
}
enum NeedsValue {
  A = "a",
  // expect-error: enum member 'B' needs a value, since the member before it is not a number
  B,
}
let notConstant = 5
enum Computed {
  // expect-error: the value of an enum member must be a constant
  A = notConstant,
  // expect-error: the value of enum member 'B' must be an integer that fits in 'int', found 2147483648
  B = 2 ** 31,
  // expect-error: found 0.5
  C = 1 / 2,
}
// expect-error: duplicate enum member 'Red'
enum Again { Red, Red }
enum Color { Red, Green }
// expect-error: property 'Purple' does not exist on enum 'Color'
const p = Color.Purple
// expect-error: enum 'Color' can only be used as 'Color.Member' or 'Color[value]'
const c = Color
enum Direction { Up = "UP" }
// expect-error: enum 'Direction' has string values, and only numbers map back to names
const name = Direction[0]

type Id = int
type Pair = Id[]
const ok: Pair = [1, 2]
// expect-error: cannot find type 'Missing'
type Broken = Missing[]
// expect-error: type alias 'Loop' refers to itself
type Loop = Loop[]
// expect-error: 'string' is a built-in type and cannot be redefined
type string = int
// expect-error: only 'type long = number', for tsc, may declare it
type long = string
type int = number
// expect-error: duplicate declaration of type 'Id'
type Id = string
// expect-error: type 'string' is not assignable to type 'int'
const wrong: Id = "x"
