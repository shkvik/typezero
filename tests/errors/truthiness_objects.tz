// What a condition cannot test about objects (docs/small.md, section 1): an
// object, an instance of a class, a class or a function without `|
// undefined` is always truthy, also after narrowing; `unknown` and an
// unconstrained T may hold a number, where 0 would count as false.
class User {
  name = "u"
}
const user = new User()
// expect-error: an object is always truthy, so the condition is always true; drop the check, or give it a type that allows undefined: 'User | undefined'
if (user) console.log(user.name)
// expect-error: an object is always truthy, so '||' never gives its right side; drop '|| ...'
const other = user || new User()
const literal = { a: 1 }
// expect-error: an object is always truthy, so '!' always gives false; drop the check, or give it a type that allows undefined: '{ a: int } | undefined'
if (!literal) console.log(literal)
// expect-error: a class is always truthy, so the condition is always true; drop the check
if (User) console.log(1)
const Kind: Constructor<User> = User
// expect-error: a class is always truthy, so '&&' always gives its right side; drop 'Kind &&'
const name = Kind && Kind.name
function check(): boolean {
  return true
}
// expect-error: a function is always truthy, so the condition is always true; call it: 'check()'
if (check) console.log(2)
const table = new Map<string, int>()
// expect-error: an object is always truthy, so the condition is always true; drop the check, or give it a type that allows undefined: 'Map<string, int> | undefined'
while (table) table.clear()
function greet(who: User | undefined, typeOf: (typeof User) | undefined) {
  if (!who) return
  // expect-error: 'who' always holds an object at this point, so the condition is always true; drop the check
  if (who) console.log(who.name)
  if (!typeOf) return
  // expect-error: 'typeOf' always holds a class at this point, so '!' always gives false; drop the check
  if (!typeOf) return
}
function value(u: unknown) {
  // expect-error: an unknown value cannot be a condition: it may hold 0, "" or false, which would count as false; check what it holds, e.g. 'u != undefined' or 'typeof u == "string"'
  if (u) console.log(1)
  // expect-error: '||' cannot test an unknown value: it may hold 0, "" or false, which would count as false; give a default with '??': 'u ?? 5'
  return u || 5
}
function firstOr<T>(xs: T[], d: T): T | undefined {
  // expect-error: '||' cannot test a value of type 'T | undefined': 'T' may be a number, and 0 would count as false; give a default with '??': 'xs[0] ?? d'
  return xs[0] || d
}
