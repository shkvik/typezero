// Unions and literal types used wrongly (docs/objects.md, section 7).
type Dir = "up" | "down" | "left"
// expect-error: type '"right"' is not assignable to type '"up" | "down" | "left"'
const right: Dir = "right"
function sideways(d: Dir) {
  // expect-error: operator '==' cannot be applied to types '"up" | "down" | "left"' and '"sideways"'
  return d == "sideways"
}

// A switch on a literal type handles every value, or says it means not to.
function step(d: Dir) {
  // expect-error: the switch does not handle "left"; add 'case "left":', or 'default: break' if leaving it out is meant
  switch (d) {
    case "up": return 1
    case "down": return 2
  }
  return 0
}
function partly(d: Dir) {
  switch (d) {
    case "up": return 1
    default:
      // expect-error: type '"down" | "left"' is not assignable to type 'never'
      const rest: never = d
      return rest
  }
}

interface Circle { kind: "circle"; r: double }
interface Square { kind: "square"; side: double }
type Shape = Circle | Square
function radius(s: Shape) {
  // expect-error: property 'r' does not exist on 'Square', which 'Circle | Square' may be; narrow it first, e.g. 'if (s.kind == "circle")'
  return s.r
}
function kinds(s: Shape) {
  // expect-error: the switch does not handle "square"
  switch (s.kind) {
    case "circle": return 1
  }
  return 2
}
// expect-error: type '{ kind: string; r: int }' is not assignable to type 'Circle | Square'; an object literal goes into a union as one of its members: it needs the fields of one of them, with its value of 'kind'
const triangle: Shape = { kind: "triangle", r: 1 }
function always(s: Shape) {
  // expect-error: operator 'in' is always true here: every member of 'Circle | Square' has field 'kind'
  return "kind" in s
}
function never2(s: Shape) {
  // expect-error: operator 'in' is always false here: no member of 'Circle | Square' has field 'z'
  return "z" in s
}

// Members that cannot be told apart when the program runs.
// expect-error: union 'int | double' is not supported: 'int' and 'double' cannot be told apart when the program runs: 'typeof' gives "number" for both
let number: int | double = 1
// expect-error: 'int[]' and 'string[]' cannot be told apart when the program runs: both are arrays; use one array type, e.g. '(int | string)[]'
let lists: int[] | string[] = [1]
// expect-error: union 'Result<int, string> | string' is not supported: a Result cannot be part of a union
let wrapped: Result<int, string> | string = "x"
// expect-error: union 'Json | string' is not supported: a Json already holds strings
let json: Json | string = "x"

// What a value of a union needs a check for.
let value: string | int = 1
// expect-error: type 'boolean' is not assignable to type 'string | int'
value = true
function twice(x: string | int) {
  // expect-error: operator '*' cannot be applied to types 'string | int' and 'int'
  return x * 2
}
function size(x: string | string[]) {
  // expect-error: property 'length' does not exist on 'string | string[]': check what it holds first, e.g. 'typeof x == "string"'
  return x.length
}
function truthy(x: string | int) {
  // expect-error: a value of type 'string | int' cannot be a condition: 'int' is a number, and 0 would count as false
  if (x) return 1
  return 0
}
function objects(s: Shape) {
  // expect-error: a value of type 'Circle | Square' is always truthy, so the condition is always true
  if (s) return 1
  return 0
}
const mixed: (string | int)[] = [2, "a"]
// expect-error: 'sort()' without a comparison function compares the values of 'string | int' as strings
mixed.sort()

class Dog { bark() { return "woof" } }
class Cat { meow() { return "meow" } }
function noise(p: Dog | Cat) {
  // expect-error: 'Dog | Cat' may be a 'Cat', which has no method 'bark'; narrow it first
  return p.bark()
}
class Fish {}
function swim(p: Dog | Cat) {
  // expect-error: a 'Dog | Cat' is never a 'Fish': none of its members is or extends it, so this is always false
  return p instanceof Fish
}
interface A { kind: "a"; v: int }
interface B { kind: "b"; v: string }
function setV(x: A | B) {
  // expect-error: the members of 'A | B' have fields 'v' of different types, 'int' and 'string', so no value fits them all; narrow it first
  x.v = 1
}
