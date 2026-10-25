// Computations over types: what they take, and the exact types they give.
interface User { id: int; name: string; email?: string }
const user: User = { id: 1, name: "ann" }
type Preview = Pick<User, "id" | "name">
// expect-error: type 'User' is not assignable to type '{ id: int; name: string }'; an object must have exactly the fields of its type, and this one has the extra field 'email'
const preview: Preview = user
// expect-error: type '"phone"' does not satisfy the constraint 'keyof T' of 'K' in 'Pick'
type Phone = Pick<User, "phone">
// expect-error: 'User' has no member '"phone"'
type Missing = User["phone"]
// expect-error: 'keyof' takes an object type, whose fields it names, not 'int'
type Numbers = keyof int
function get<T, K extends keyof T>(o: T, k: K): T[K] { return o[k] }
// expect-error: type '"phone"' does not satisfy the constraint 'keyof T' of 'K' in 'get'
get(user, "phone")
function bad<T>(o: T, k: string) {
  // expect-error: 'T' is indexed by its keys, such as 'k: K' with 'K extends keyof T', not by 'string'
  return o[k]
}
// expect-error: 'ReturnType' takes a function type, not 'string'
type NoFunction = ReturnType<string>
// expect-error: the keys of a mapped type are names of fields, string literal types such as '"id" | "name"' or 'keyof T', not 'int'
type ByNumber = { [K in int]: string }
