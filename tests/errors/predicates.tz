// Functions that test, written wrongly (docs/narrowing.md, section 2).
interface Circle { kind: "circle"; r: double }
interface Square { kind: "square"; side: double }
type Shape = Circle | Square

// A written `x is T` must be what each return that may give true shows.
// expect-error: function 'lies' says 's is Circle', but this may give true while 's' is only known to be 'Square'
function lies(s: Shape): s is Circle { return s.kind == "square" }
function partly(s: Shape): s is Circle {
  if (s.kind == "circle") return true
  // expect-error: function 'partly' says 's is Circle', but this may give true while 's' is only known to be 'Square'
  return true
}
// expect-error: a type predicate names a parameter of the function, and 'q' is none
function wrongName(s: Shape): q is Circle { return true }
function assertsBad(s: Shape): asserts s is Circle {
  // expect-error: function 'assertsBad' says 'asserts s is Circle', but it may end here while 's' is only known to be 'Square'
  if (s.kind == "square") return
}
// expect-error: 'this is T' can only be the return type of a method
function notMethod(s: Shape): this is Circle { return true }

// A function proves no more than its body shows.
function loose(s: Shape) {
  console.log("checking")
  return s.kind == "circle" || s.kind == "square"
}
function useLoose(s: Shape) {
  // expect-error: property 'r' does not exist on 'Square'
  if (loose(s)) return s.r
  return 0
}
// A fact about a field is forgotten when the field may change.
interface Person { name: string; email?: string }
function hasEmail(p: Person) { return p.email != undefined }
function clear(p: Person) { p.email = undefined }
function send(p: Person) {
  if (hasEmail(p)) {
    clear(p)
    // expect-error: type 'string | undefined' is not assignable to type 'string'
    const email: string = p.email
    return email
  }
  return ""
}
